/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * mbedTLS backend for libcurl - Xbox 360 port
 * Based on the PolarSSL backend from libcurl 7.37.1, adapted for
 * mbedTLS 2.28 API.
 *
 ***************************************************************************/

#include "curl_setup.h"

#ifdef _XBOX
extern void DbgPrint(const char *fmt, ...);
#if !defined(X360_RUNTIME_PROBES) || !X360_RUNTIME_PROBES
/* Xbox 360 诊断探针总开关（默认 OFF）：关闭时本文件的 [tls] 探针编译为空。 */
#define DbgPrint(...) ((void)0)
#endif
#endif

#ifdef USE_MBEDTLS

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/version.h>

#ifdef _XBOX
#include <xtl.h>
#include <winsockx.h>
#else
#include <mbedtls/net_sockets.h>
#endif

#include "urldata.h"
#include "sendf.h"
#include "inet_pton.h"
#include "mbedtls.h"
#include "vtls.h"
#include "connect.h"
#include "select.h"
#include "rawstr.h"

#define _MPRINTF_REPLACE
#include <curl/mprintf.h>
#include "curl_memory.h"
/* The last #include file should be: */
#include "memdebug.h"

static Curl_recv mbedtls_recv_func;
static Curl_send mbedtls_send_func;

#ifdef _XBOX
/* P0 §5.5-1 测量：握手 enter/exit 用的单调毫钟（QPC）。 */
static unsigned long xbox_tls_now_ms(void)
{
  static LARGE_INTEGER freq = {0};
  LARGE_INTEGER now;
  if(freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
    if(freq.QuadPart == 0)
      freq.QuadPart = 1;
  }
  QueryPerformanceCounter(&now);
  return (unsigned long)((double)now.QuadPart * 1000.0 / (double)freq.QuadPart);
}

/* 五轮：线程"实际执行"时间（GetThreadTimes 的 kernel time——Xbox 上所有标题
   线程都跑在 kernel mode，user time 恒为 0，故 kernel time 即总执行时间，
   且不含阻塞/等待，见 XDK "Thread Times"）。用来判定 15s 握手到底是
   CPU 在算，还是在等某个锁/内核对象。单位 100ns。 */
static unsigned long long xbox_thread_cpu_100ns(void)
{
  FILETIME creation, exit_t, kernel, user;
  if(!GetThreadTimes(GetCurrentThread(), &creation, &exit_t, &kernel, &user))
    return 0;
  return ((unsigned long long)kernel.dwHighDateTime << 32) |
         (unsigned long long)kernel.dwLowDateTime;
}
#endif

#ifdef _XBOX
static int mbedtls_x509_crt_parse_xbox_file(
  mbedtls_x509_crt *chain,
  const char *path)
{
  static const long maximum_bundle_size = 1024L * 1024L;
  FILE *file;
  long size;
  unsigned char *contents;
  size_t read_size;
  int result;

  file = fopen(path, "rb");
  if(!file)
    return MBEDTLS_ERR_X509_FILE_IO_ERROR;
  if(fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return MBEDTLS_ERR_X509_FILE_IO_ERROR;
  }
  size = ftell(file);
  if(size <= 0 || size > maximum_bundle_size ||
     fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return MBEDTLS_ERR_X509_FILE_IO_ERROR;
  }
  contents = malloc((size_t)size + 1);
  if(!contents) {
    fclose(file);
    return MBEDTLS_ERR_X509_ALLOC_FAILED;
  }
  read_size = fread(contents, 1, (size_t)size, file);
  fclose(file);
  if(read_size != (size_t)size) {
    free(contents);
    return MBEDTLS_ERR_X509_FILE_IO_ERROR;
  }
  contents[size] = '\0';
  result = mbedtls_x509_crt_parse(chain, contents, (size_t)size + 1);
  memset(contents, 0, (size_t)size + 1);
  free(contents);
  return result;
}
#endif

/*
 * Custom BIO callbacks for mbedTLS - wraps recv()/send() on the curl socket.
 * Xbox 360 doesn't have MBEDTLS_NET_C (no _WIN32/unix), so we provide our own.
 */
#ifdef _XBOX
/* P0 §5.6 四轮测量：按 fd 累计【握手窗口内】bio recv/send 的总等待与调用次数。
   三轮实测 hs 耗时 15~21s 但单次 recv stall(≥1s)=0，需要区分三种可能：
     (a) 大量 <1s 的涓流 recv（recv_n 大、recv_ms 接近总耗时）
     (b) 阻塞 send（原先完全没计时）
     (c) CPU/加密争抢（recv_ms+send_ms 都远小于 hs dur）
   槽只在 hs enter 时创建、hs exit 时清除，因此传输阶段的高频 recv/send 不加锁。 */
#define XBOX_BIO_SLOTS 64
/* 五轮：把握手窗口内的时间按 TLS 状态机状态归因（mbedtls 2.28 枚举 19 项，
   上限留 24）。四轮结论是 recv/send 几乎不等待但 hs 仍要 15~25s，说明时间
   花在两次收发【之间】的状态处理里——状态归因直接指出是哪个状态。 */
#define XBOX_SSL_STATE_N 24
typedef struct {
  int fd;                  /* 0 = 空槽（Xbox 上 socket fd 不会是 0） */
  unsigned long recv_n;
  unsigned long recv_ms;
  unsigned long send_n;
  unsigned long send_ms;
  unsigned long t0;                /* hs enter，用于把 io 时刻相对化 */
  unsigned long first_send;        /* 相对 ms；0xFFFFFFFF = 未发生 */
  unsigned long last_send;
  unsigned long first_recv;
  unsigned long last_recv;
  void *drbg;                      /* RNG 归因键（= &connssl->ctr_drbg） */
  unsigned long rng_n;
  unsigned long rng_ms;
  unsigned long long cpu0;         /* hs enter 时的线程内核态(=实际执行)时间 */
  unsigned long state_ms[XBOX_SSL_STATE_N];
} xbox_bio_stat;

static xbox_bio_stat xbox_bio_stats[XBOX_BIO_SLOTS];
static volatile LONG xbox_bio_guard = 0;
/* 活跃握手窗口数：为 0 时（传输/空闲阶段）所有 add 直接返回，
   真正做到"高频路径不加锁"——四轮实现其实每次 recv/send 都会取锁查表。 */
static volatile LONG xbox_bio_active = 0;

static void xbox_bio_lock(void)
{
  while(InterlockedCompareExchange(&xbox_bio_guard, 1, 0) != 0)
    Sleep(0);
}
static void xbox_bio_unlock(void)
{
  InterlockedExchange(&xbox_bio_guard, 0);
}

/* 调用方必须已持锁。create=1 时允许占用空槽。找不到返回 -1。 */
static int xbox_bio_slot_locked(int fd, int create)
{
  int i, free_slot = -1;
  for(i = 0; i < XBOX_BIO_SLOTS; i++) {
    if(xbox_bio_stats[i].fd == fd)
      return i;
    if(free_slot < 0 && xbox_bio_stats[i].fd == 0)
      free_slot = i;
  }
  if(create && free_slot >= 0) {
    int s;
    memset(&xbox_bio_stats[free_slot], 0, sizeof(xbox_bio_stats[free_slot]));
    xbox_bio_stats[free_slot].fd = fd;
    xbox_bio_stats[free_slot].first_send = 0xFFFFFFFFUL;
    xbox_bio_stats[free_slot].last_send = 0xFFFFFFFFUL;
    xbox_bio_stats[free_slot].first_recv = 0xFFFFFFFFUL;
    xbox_bio_stats[free_slot].last_recv = 0xFFFFFFFFUL;
    for(s = 0; s < XBOX_SSL_STATE_N; s++)
      xbox_bio_stats[free_slot].state_ms[s] = 0;
    return free_slot;
  }
  return -1;
}

/* hs enter：为该 fd 开一个统计窗口（t0=enter 时刻，drbg=RNG 归因键）。 */
static void xbox_bio_begin(int fd, unsigned long t0, void *drbg,
                           unsigned long long cpu0)
{
  int slot;
  xbox_bio_lock();
  slot = xbox_bio_slot_locked(fd, 1);
  if(slot >= 0) {
    xbox_bio_stats[slot].t0 = t0;
    xbox_bio_stats[slot].drbg = drbg;
    xbox_bio_stats[slot].cpu0 = cpu0;
    InterlockedIncrement(&xbox_bio_active);
  }
  xbox_bio_unlock();
}

/* recv/send 后累计；槽不存在（非握手窗口）则什么都不做，无锁开销。
   now 用于记录 io 发生在握手开始后的第几个毫秒——四轮只测了"耗时"，
   没测"何时发生"，五轮补上（例如 send@12000ms 即首包拖了 12s 才发）。 */
static void xbox_bio_add(int fd, int is_send, unsigned long dt, unsigned long now)
{
  int slot;
  if(xbox_bio_active == 0)
    return; /* 无握手窗口：传输阶段，零开销 */
  xbox_bio_lock();
  slot = xbox_bio_slot_locked(fd, 0);
  if(slot >= 0) {
    unsigned long rel = (now >= xbox_bio_stats[slot].t0)
                            ? now - xbox_bio_stats[slot].t0
                            : 0;
    if(is_send) {
      xbox_bio_stats[slot].send_n++;
      xbox_bio_stats[slot].send_ms += dt;
      if(xbox_bio_stats[slot].send_n == 1)
        xbox_bio_stats[slot].first_send = rel;
      xbox_bio_stats[slot].last_send = rel;
    }
    else {
      xbox_bio_stats[slot].recv_n++;
      xbox_bio_stats[slot].recv_ms += dt;
      if(xbox_bio_stats[slot].recv_n == 1)
        xbox_bio_stats[slot].first_recv = rel;
      xbox_bio_stats[slot].last_recv = rel;
    }
  }
  xbox_bio_unlock();
}

/* 一次握手状态机 step 的耗时归因。 */
static void xbox_bio_state_add(int fd, int st, unsigned long dt)
{
  int slot;
  if(st < 0 || st >= XBOX_SSL_STATE_N)
    return; /* 枚举越界：不加锁，直接丢弃 */
  if(xbox_bio_active == 0)
    return;
  xbox_bio_lock();
  slot = xbox_bio_slot_locked(fd, 0);
  if(slot >= 0)
    xbox_bio_stats[slot].state_ms[st] += dt;
  xbox_bio_unlock();
}

/* f_rng 调用耗时（按 ctr_drbg 指针归因到对应 fd 的窗口）。 */
static void xbox_bio_rng_add(void *drbg, unsigned long dt)
{
  int i;
  if(!drbg)
    return;
  if(xbox_bio_active == 0)
    return; /* 传输阶段的 RNG 调用（若有）不取锁 */
  xbox_bio_lock();
  for(i = 0; i < XBOX_BIO_SLOTS; i++) {
    if(xbox_bio_stats[i].fd != 0 && xbox_bio_stats[i].drbg == drbg) {
      xbox_bio_stats[i].rng_n++;
      xbox_bio_stats[i].rng_ms += dt;
      break;
    }
  }
  xbox_bio_unlock();
}

static const char *xbox_ssl_state_name(int st)
{
  switch(st) {
    case MBEDTLS_SSL_HELLO_REQUEST: return "HELLO_REQ";
    case MBEDTLS_SSL_CLIENT_HELLO: return "CLIENT_HELLO";
    case MBEDTLS_SSL_SERVER_HELLO: return "SERVER_HELLO";
    case MBEDTLS_SSL_SERVER_CERTIFICATE: return "CERT";
    case MBEDTLS_SSL_SERVER_KEY_EXCHANGE: return "SKE";
    case MBEDTLS_SSL_CERTIFICATE_REQUEST: return "CERT_REQ";
    case MBEDTLS_SSL_SERVER_HELLO_DONE: return "SHD";
    case MBEDTLS_SSL_CLIENT_CERTIFICATE: return "CL_CERT";
    case MBEDTLS_SSL_CLIENT_KEY_EXCHANGE: return "CKE";
    case MBEDTLS_SSL_CERTIFICATE_VERIFY: return "CERT_VERIFY";
    case MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC: return "CL_CCS";
    case MBEDTLS_SSL_CLIENT_FINISHED: return "CL_FIN";
    case MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC: return "SV_CCS";
    case MBEDTLS_SSL_SERVER_FINISHED: return "SV_FIN";
    case MBEDTLS_SSL_FLUSH_BUFFERS: return "FLUSH";
    case MBEDTLS_SSL_HANDSHAKE_WRAPUP: return "WRAPUP";
    case MBEDTLS_SSL_HANDSHAKE_OVER: return "OVER";
    case MBEDTLS_SSL_SERVER_NEW_SESSION_TICKET: return "TICKET";
    case MBEDTLS_SSL_SERVER_HELLO_VERIFY_REQUEST_SENT: return "HVR";
    default: return "ST?";
  }
}

/* hs 结束（成功/失败/超时均调用）：取出并清除窗口，打统计行。
   第一行是四轮的 recv/send 汇总；第二行是五轮新增的状态归因 + RNG +
   io 时刻——用于把 15~25s 的"无解释时间"定位到具体状态。 */
static void xbox_bio_finish(int fd, const char *tag, unsigned long dur_ms,
                            unsigned long iters)
{
  unsigned long rn = 0, rms = 0, sn = 0, sms = 0;
  unsigned long rng_n = 0, rng_ms = 0;
  unsigned long long cpu0 = 0, cpu_ms = 0;
  unsigned long first_send = 0xFFFFFFFFUL, last_send = 0xFFFFFFFFUL;
  unsigned long first_recv = 0xFFFFFFFFUL, last_recv = 0xFFFFFFFFUL;
  unsigned long states[XBOX_SSL_STATE_N];
  char buf[320];
  int slot, i, off = 0;
  memset(states, 0, sizeof(states));
  xbox_bio_lock();
  slot = xbox_bio_slot_locked(fd, 0);
  if(slot >= 0) {
    rn = xbox_bio_stats[slot].recv_n;
    rms = xbox_bio_stats[slot].recv_ms;
    sn = xbox_bio_stats[slot].send_n;
    sms = xbox_bio_stats[slot].send_ms;
    rng_n = xbox_bio_stats[slot].rng_n;
    rng_ms = xbox_bio_stats[slot].rng_ms;
    first_send = xbox_bio_stats[slot].first_send;
    last_send = xbox_bio_stats[slot].last_send;
    first_recv = xbox_bio_stats[slot].first_recv;
    last_recv = xbox_bio_stats[slot].last_recv;
    cpu0 = xbox_bio_stats[slot].cpu0;
    for(i = 0; i < XBOX_SSL_STATE_N; i++)
      states[i] = xbox_bio_stats[slot].state_ms[i];
    xbox_bio_stats[slot].fd = 0;
    xbox_bio_stats[slot].drbg = NULL;
    InterlockedDecrement(&xbox_bio_active);
  }
  xbox_bio_unlock();
  if(cpu0 != 0) {
    const unsigned long long cpu1 = xbox_thread_cpu_100ns();
    cpu_ms = (cpu1 > cpu0) ? (cpu1 - cpu0) / 10000ULL : 0;
  }
  DbgPrint("[tls] fd=%d hs-%s dur=%lu ms cpu=%ld ms iters=%lu | recv n=%lu wait=%lu ms | send n=%lu wait=%lu ms",
           fd, tag, dur_ms, cpu0 ? (long)cpu_ms : -1L, iters, rn, rms, sn, sms);
  for(i = 0; i < XBOX_SSL_STATE_N && off < (int)sizeof(buf) - 32; i++) {
    if(states[i] >= 10)
      off += snprintf(buf + off, sizeof(buf) - off, " %s=%lu",
                      xbox_ssl_state_name(i), states[i]);
  }
  if(off == 0)
    snprintf(buf, sizeof(buf), " (none>=10ms)");
  DbgPrint("[tls] fd=%d hs-%s st:%s | rng n=%lu ms=%lu | io f_send=%ld l_send=%ld f_recv=%ld l_recv=%ld",
           fd, tag, buf, rng_n, rng_ms, (long)first_send, (long)last_send,
           (long)first_recv, (long)last_recv);
}
#endif

#ifdef _XBOX
/* 五轮：f_rng 包一层计时。ctr_drbg_seed 在 step1（窗口外）也走这里，
   但握手期的 client random / ECDHE 私钥生成都在窗口内，若熵源或 RNG
   阻塞，rng_ms 会直接暴露出来。 */
static int xbox_rng_wrap(void *p_rng, unsigned char *output, size_t output_len)
{
  const unsigned long t0 = xbox_tls_now_ms();
  int ret = mbedtls_ctr_drbg_random(p_rng, output, output_len);
  xbox_bio_rng_add(p_rng, xbox_tls_now_ms() - t0);
  return ret;
}
#endif

static int mbedtls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
  curl_socket_t fd = *(curl_socket_t *)ctx;
#ifdef _XBOX
  /* P0 §5.5-1/#8 + §5.6-2：单次阻塞等待。三轮阈值 1000ms 计数为 0（存疑），
     四轮降到 200ms，并同时按 fd 累计到 xbox_bio_stats（不依赖逐行打印）。 */
  const unsigned long t0 = xbox_tls_now_ms();
#endif
  int ret = recv((SOCKET)fd, (char *)buf, (int)len, 0);
#ifdef _XBOX
  {
    const unsigned long t1 = xbox_tls_now_ms();
    const unsigned long dt = t1 - t0;
    if(dt >= 200)
      DbgPrint("[tls] fd=%d recv stall %lu ms ret=%d", (int)fd, dt, ret);
    xbox_bio_add((int)fd, 0, dt, t1);
  }
#endif
  if(ret < 0) {
#ifdef _XBOX
    int err = WSAGetLastError();
    DbgPrint("[tls] fd=%d recv ret=%d err=%d", (int)fd, ret, err);
    if(err == WSAEWOULDBLOCK)
      return MBEDTLS_ERR_SSL_WANT_READ;
#endif
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return ret;
}

static int mbedtls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
  curl_socket_t fd = *(curl_socket_t *)ctx;
#ifdef _XBOX
  /* §5.6-2 新增：send 此前完全没计时，是"hs 慢但 recv 不慢"的头号嫌疑。 */
  const unsigned long t0 = xbox_tls_now_ms();
#endif
  int ret = send((SOCKET)fd, (const char *)buf, (int)len, 0);
#ifdef _XBOX
  {
    const unsigned long t1 = xbox_tls_now_ms();
    const unsigned long dt = t1 - t0;
    if(dt >= 200)
      DbgPrint("[tls] fd=%d send stall %lu ms ret=%d", (int)fd, dt, ret);
    xbox_bio_add((int)fd, 1, dt, t1);
  }
#endif
  if(ret < 0) {
#ifdef _XBOX
    int err = WSAGetLastError();
    DbgPrint("[tls] fd=%d send ret=%d err=%d", (int)fd, ret, err);
    if(err == WSAEWOULDBLOCK)
      return MBEDTLS_ERR_SSL_WANT_WRITE;
#endif
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return ret;
}

static void mbedtls_free_connection(struct ssl_connect_data *connssl)
{
  if(!connssl->backend_initialized)
    return;

  mbedtls_pk_free(&connssl->pk);
  mbedtls_x509_crt_free(&connssl->clicert);
  mbedtls_x509_crt_free(&connssl->cacert);
  mbedtls_ssl_free(&connssl->ssl);
  mbedtls_ssl_config_free(&connssl->config);
  mbedtls_ctr_drbg_free(&connssl->ctr_drbg);
  mbedtls_entropy_free(&connssl->entropy);
  connssl->backend_initialized = FALSE;
}

int Curl_mbedtls_init(void)
{
  return 1; /* nothing to do globally */
}

void Curl_mbedtls_cleanup(void)
{
  /* All state is owned by individual connections. */
}

static CURLcode
mbedtls_connect_step1(struct connectdata *conn, int sockindex)
{
  struct SessionHandle *data = conn->data;
  curl_socket_t sockfd = conn->sock[sockindex];
  struct ssl_connect_data *connssl = &conn->ssl[sockindex];
  int ret;

  if(connssl->backend_initialized)
    mbedtls_free_connection(connssl);

  /* Initialize mbedTLS structures */
  mbedtls_entropy_init(&connssl->entropy);
  mbedtls_ssl_init(&connssl->ssl);
  mbedtls_ssl_config_init(&connssl->config);
  mbedtls_ctr_drbg_init(&connssl->ctr_drbg);
  mbedtls_x509_crt_init(&connssl->cacert);
  mbedtls_x509_crt_init(&connssl->clicert);
  mbedtls_pk_init(&connssl->pk);
  connssl->backend_initialized = TRUE;

  /* Seed the RNG */
  ret = mbedtls_ctr_drbg_seed(&connssl->ctr_drbg, mbedtls_entropy_func,
                               &connssl->entropy, NULL, 0);
  if(ret) {
    failf(data, "mbedTLS: ctr_drbg_seed returned (-0x%04X)", -ret);
    mbedtls_free_connection(connssl);
    return CURLE_SSL_CONNECT_ERROR;
  }

  /* Set up SSL config - TLS client defaults */
  ret = mbedtls_ssl_config_defaults(&connssl->config,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
  if(ret) {
    failf(data, "mbedTLS: ssl_config_defaults returned (-0x%04X)", -ret);
    mbedtls_free_connection(connssl);
    return CURLE_SSL_CONNECT_ERROR;
  }

  /* Set TLS version range based on curl options */
  switch(data->set.ssl.version) {
  case CURL_SSLVERSION_DEFAULT:
  case CURL_SSLVERSION_TLSv1:
    mbedtls_ssl_conf_min_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_1);
    break;
  case CURL_SSLVERSION_TLSv1_0:
    mbedtls_ssl_conf_min_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_1);
    mbedtls_ssl_conf_max_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_1);
    break;
  case CURL_SSLVERSION_TLSv1_1:
    mbedtls_ssl_conf_min_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_2);
    mbedtls_ssl_conf_max_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_2);
    break;
  case CURL_SSLVERSION_TLSv1_2:
    mbedtls_ssl_conf_min_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&connssl->config,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_3);
    break;
  case CURL_SSLVERSION_SSLv2:
  case CURL_SSLVERSION_SSLv3:
    failf(data, "mbedTLS does not support SSLv2 or SSLv3");
    mbedtls_free_connection(connssl);
    return CURLE_SSL_CONNECT_ERROR;
  }

  /* Certificate verification uses the caller-provided CA bundle. */
  if(data->set.ssl.verifypeer) {
    mbedtls_ssl_conf_authmode(&connssl->config, MBEDTLS_SSL_VERIFY_REQUIRED);
  }
  else {
    mbedtls_ssl_conf_authmode(&connssl->config, MBEDTLS_SSL_VERIFY_NONE);
  }

#if defined(MBEDTLS_FS_IO) || defined(_XBOX)
  /* CA certs from file if specified */
  if(data->set.str[STRING_SSL_CAFILE]) {
#ifdef _XBOX
    ret = mbedtls_x509_crt_parse_xbox_file(&connssl->cacert,
                                            data->set.str[STRING_SSL_CAFILE]);
#else
    ret = mbedtls_x509_crt_parse_file(&connssl->cacert,
                                       data->set.str[STRING_SSL_CAFILE]);
#endif
    if(ret < 0) {
      failf(data, "mbedTLS: x509_crt_parse_file returned (-0x%04X)", -ret);
      if(data->set.ssl.verifypeer) {
        mbedtls_free_connection(connssl);
        return CURLE_SSL_CACERT_BADFILE;
      }
    }
    mbedtls_ssl_conf_ca_chain(&connssl->config, &connssl->cacert, NULL);
  }
#endif

#ifdef MBEDTLS_FS_IO
  /* Client cert if specified */
  if(data->set.str[STRING_CERT]) {
    ret = mbedtls_x509_crt_parse_file(&connssl->clicert,
                                       data->set.str[STRING_CERT]);
    if(ret) {
      failf(data, "mbedTLS: x509_crt_parse_file (client) returned (-0x%04X)",
            -ret);
      mbedtls_free_connection(connssl);
      return CURLE_SSL_CERTPROBLEM;
    }
  }

  /* Client private key */
  if(data->set.str[STRING_KEY]) {
    ret = mbedtls_pk_parse_keyfile(&connssl->pk,
                                    data->set.str[STRING_KEY],
                                    data->set.str[STRING_KEY_PASSWD]);
    if(ret) {
      failf(data, "mbedTLS: pk_parse_keyfile returned (-0x%04X)", -ret);
      mbedtls_free_connection(connssl);
      return CURLE_SSL_CERTPROBLEM;
    }
    mbedtls_ssl_conf_own_cert(&connssl->config, &connssl->clicert,
                               &connssl->pk);
  }
#endif /* MBEDTLS_FS_IO */

  /* Set RNG */
#ifdef _XBOX
  mbedtls_ssl_conf_rng(&connssl->config, xbox_rng_wrap, &connssl->ctr_drbg);
#else
  mbedtls_ssl_conf_rng(&connssl->config, mbedtls_ctr_drbg_random,
                        &connssl->ctr_drbg);
#endif

  /* Apply config to SSL context */
  ret = mbedtls_ssl_setup(&connssl->ssl, &connssl->config);
  if(ret) {
    failf(data, "mbedTLS: ssl_setup returned (-0x%04X)", -ret);
    mbedtls_free_connection(connssl);
    return CURLE_SSL_CONNECT_ERROR;
  }

  /* Set hostname for SNI */
  if(conn->host.name) {
    struct in_addr addr4;
#ifdef ENABLE_IPV6
    struct in6_addr addr6;
#endif
    /* Only set SNI for hostnames, not IP addresses */
    if(!Curl_inet_pton(AF_INET, conn->host.name, &addr4)
#ifdef ENABLE_IPV6
       && !Curl_inet_pton(AF_INET6, conn->host.name, &addr6)
#endif
       ) {
      ret = mbedtls_ssl_set_hostname(&connssl->ssl, conn->host.name);
      if(ret) {
        failf(data, "mbedTLS: ssl_set_hostname returned (-0x%04X)", -ret);
        mbedtls_free_connection(connssl);
        return CURLE_SSL_CONNECT_ERROR;
      }
    }
  }

  /* Set I/O callbacks - use custom BIO wrapping curl's socket */
  mbedtls_ssl_set_bio(&connssl->ssl, &conn->sock[sockindex],
                       mbedtls_bio_send, mbedtls_bio_recv, NULL);

#ifdef _XBOX
  /* XNet's ioctlsocket is the authoritative FIONBIO path.  The generic
     curlx_nonblock wrapper can select an unavailable CRT branch on this port,
     leaving the fd non-blocking and making mbedTLS spin on WSAEWOULDBLOCK. */
  {
    int timeout = 10000;
    unsigned long blocking = 0;
    int nbret;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&timeout, sizeof(timeout));
    nbret = ioctlsocket(sockfd, FIONBIO, &blocking);
    DbgPrint("[tls] fd=%d blocking ioctlsocket ret=%d err=%d",
             (int)sockfd, nbret, nbret ? WSAGetLastError() : 0);
  }
#endif

  infof(data, "mbedTLS: Connecting to %s:%d\n",
        conn->host.name, conn->remote_port);

  connssl->connecting_state = ssl_connect_2;
  return CURLE_OK;
}

static CURLcode
mbedtls_connect_step2(struct connectdata *conn, int sockindex)
{
  int ret;
#ifdef _XBOX
  unsigned xbox_want_retries = 0;
  /* §5.6-2：握手循环迭代数。若 iters 很大 → 时间花在 WANT_READ Sleep(10)
     重试；若 iters=1 而 dur=15s → 全部时间在单次 mbedtls_ssl_handshake 内。 */
  unsigned long xbox_hs_iters = 0;
#endif
  struct SessionHandle *data = conn->data;
  struct ssl_connect_data *connssl = &conn->ssl[sockindex];

  conn->recv[sockindex] = mbedtls_recv_func;
  conn->send[sockindex] = mbedtls_send_func;

#ifdef _XBOX
  /* P0 §5.5-1 测量：每个 fd 的握手 enter/exit 时间戳，用来验证"全局串行化"
     假设——若 A 的 enter→exit 区间与 B 的 enter 重叠，则握手是并发的；
     若 B 的 enter 总在 A 的 exit 之后，说明存在全局串行（锁/单线程限制）。
     t 使用单调毫钟（QPC），跨行可直接相减。 */
  const unsigned long hs_enter_t = xbox_tls_now_ms();
  const unsigned long long hs_enter_cpu = xbox_thread_cpu_100ns();
  DbgPrint("[tls] fd=%d hs enter t=%lu", (int)conn->sock[sockindex], hs_enter_t);
  xbox_bio_begin((int)conn->sock[sockindex], hs_enter_t,
                 &connssl->ctr_drbg, hs_enter_cpu);
#endif

  for(;;) {
#ifdef _XBOX
    xbox_hs_iters++;
#endif
#ifdef _XBOX
    /* 五轮：不再直接调 mbedtls_ssl_handshake（其内部就是下面这个循环），
       改为自己逐步 step，把每步耗时归因到 TLS 状态机状态。四轮已证明
       recv/send 几乎不等待（<100ms），但 hs 仍要 15~25s——差额必然落在
       两次收发之间的状态处理里（解析/验签/RNG/内存）。语义与
       mbedtls_ssl_handshake 完全一致：state 到 OVER 或 step 出错即停。 */
    ret = 0;
    while(connssl->ssl.state != MBEDTLS_SSL_HANDSHAKE_OVER) {
      const int st = (int)connssl->ssl.state;
      const unsigned long st_t0 = xbox_tls_now_ms();
      ret = mbedtls_ssl_handshake_step(&connssl->ssl);
      xbox_bio_state_add((int)conn->sock[sockindex], st,
                         xbox_tls_now_ms() - st_t0);
      if(ret != 0)
        break;
    }
#else
    ret = mbedtls_ssl_handshake(&connssl->ssl);
#endif
    if(ret == 0)
      break;

#ifdef _XBOX
    DbgPrint("[tls] fd=%d handshake ret=-0x%04X", (int)conn->sock[sockindex], -ret);
#endif

    if(ret != MBEDTLS_ERR_SSL_WANT_READ &&
       ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      char errorbuf[128];
      mbedtls_strerror(ret, errorbuf, sizeof(errorbuf));
      failf(data, "mbedTLS: ssl_handshake returned (-0x%04X) %s",
            -ret, errorbuf);
      mbedtls_free_connection(connssl);
#ifdef _XBOX
      DbgPrint("[tls] fd=%d hs fail t=%lu dur=%lu ms ret=-0x%04X",
               (int)conn->sock[sockindex], xbox_tls_now_ms(),
               xbox_tls_now_ms() - hs_enter_t, -ret);
      xbox_bio_finish((int)conn->sock[sockindex], "fail",
                      xbox_tls_now_ms() - hs_enter_t, xbox_hs_iters);
#endif
      return CURLE_SSL_CONNECT_ERROR;
    }

    if(ret == MBEDTLS_ERR_SSL_WANT_READ) {
#ifdef _XBOX
      /* The devkit Winsock layer can still report WSAEWOULDBLOCK after the
         socket was switched to blocking mode.  Keep the handshake in this
         call so curl does not park a half-open TLS connection and create a
         new socket for every outer state-machine tick. */
      if(++xbox_want_retries >= 1000) {
        failf(data, "mbedTLS: Xbox handshake timed out waiting for peer");
        DbgPrint("[tls] fd=%d hs timeout-read t=%lu dur=%lu ms", (int)conn->sock[sockindex],
                 xbox_tls_now_ms(), xbox_tls_now_ms() - hs_enter_t);
        xbox_bio_finish((int)conn->sock[sockindex], "to-read",
                        xbox_tls_now_ms() - hs_enter_t, xbox_hs_iters);
        mbedtls_free_connection(connssl);
        return CURLE_OPERATION_TIMEDOUT;
      }
      Sleep(10);
      connssl->connecting_state = ssl_connect_2;
      continue;
#else
      connssl->connecting_state = ssl_connect_2_reading;
      return CURLE_OK;
#endif
    }
    if(ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
#ifdef _XBOX
      if(++xbox_want_retries >= 1000) {
        failf(data, "mbedTLS: Xbox handshake timed out waiting to write");
        DbgPrint("[tls] fd=%d hs timeout-write t=%lu dur=%lu ms", (int)conn->sock[sockindex],
                 xbox_tls_now_ms(), xbox_tls_now_ms() - hs_enter_t);
        xbox_bio_finish((int)conn->sock[sockindex], "to-write",
                        xbox_tls_now_ms() - hs_enter_t, xbox_hs_iters);
        mbedtls_free_connection(connssl);
        return CURLE_OPERATION_TIMEDOUT;
      }
      Sleep(10);
      connssl->connecting_state = ssl_connect_2;
      continue;
#else
      connssl->connecting_state = ssl_connect_2_writing;
      return CURLE_OK;
#endif
    }
  }

#ifdef _XBOX
  /* P0 §5.5-1 测量：exit 带 enter→exit 耗时，与其它 fd 的 enter 行对照
     即可判定并发/串行；失败路径在下面各 return 前补打。 */
  DbgPrint("[tls] fd=%d hs exit t=%lu dur=%lu ms cs=%s", (int)conn->sock[sockindex],
           xbox_tls_now_ms(), xbox_tls_now_ms() - hs_enter_t,
           mbedtls_ssl_get_ciphersuite(&connssl->ssl) ?
               mbedtls_ssl_get_ciphersuite(&connssl->ssl) : "?");
  xbox_bio_finish((int)conn->sock[sockindex], "exit",
                  xbox_tls_now_ms() - hs_enter_t, xbox_hs_iters);
#endif

  /* Check certificate verification result */
  if(data->set.ssl.verifypeer) {
    uint32_t flags = mbedtls_ssl_get_verify_result(&connssl->ssl);
    if(flags != 0) {
      if(flags & MBEDTLS_X509_BADCERT_EXPIRED)
        failf(data, "Cert verify failed: BADCERT_EXPIRED");
      if(flags & MBEDTLS_X509_BADCERT_REVOKED)
        failf(data, "Cert verify failed: BADCERT_REVOKED");
      if(flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
        failf(data, "Cert verify failed: BADCERT_CN_MISMATCH");
      if(flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
        failf(data, "Cert verify failed: BADCERT_NOT_TRUSTED");
      mbedtls_free_connection(connssl);
      return CURLE_PEER_FAILED_VERIFICATION;
    }
  }

  infof(data, "mbedTLS: Handshake complete, cipher is %s\n",
        mbedtls_ssl_get_ciphersuite(&connssl->ssl));

#ifdef _XBOX
  /* Keep the socket blocking for the whole curl transfer.  XNet's readiness
     reporting is unreliable for TLS records; restoring non-blocking mode here
     causes recv() to return WSAEWOULDBLOCK indefinitely and leaks sockets
     while callers retry.  SO_RCVTIMEO/SO_SNDTIMEO bound stalled transfers. */
  DbgPrint("[tls] fd=%d keeping blocking mode after handshake",
           (int)conn->sock[sockindex]);
#endif

  connssl->connecting_state = ssl_connect_3;
  infof(data, "SSL connected\n");

  return CURLE_OK;
}

static CURLcode
mbedtls_connect_step3(struct connectdata *conn, int sockindex)
{
  struct ssl_connect_data *connssl = &conn->ssl[sockindex];

  DEBUGASSERT(ssl_connect_3 == connssl->connecting_state);

  connssl->connecting_state = ssl_connect_done;
  return CURLE_OK;
}

static ssize_t mbedtls_send_func(struct connectdata *conn,
                                  int sockindex,
                                  const void *mem,
                                  size_t len,
                                  CURLcode *curlcode)
{
  int ret;

  ret = mbedtls_ssl_write(&conn->ssl[sockindex].ssl,
                           (unsigned char *)mem, len);
  if(ret < 0) {
    *curlcode = (ret == MBEDTLS_ERR_SSL_WANT_WRITE) ?
      CURLE_AGAIN : CURLE_SEND_ERROR;
    ret = -1;
  }

  return (ssize_t)ret;
}

static ssize_t mbedtls_recv_func(struct connectdata *conn,
                                  int num,
                                  char *buf,
                                  size_t buffersize,
                                  CURLcode *curlcode)
{
  int ret;

  ret = mbedtls_ssl_read(&conn->ssl[num].ssl,
                          (unsigned char *)buf, buffersize);

  if(ret <= 0) {
    if(ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
      return 0;

    *curlcode = (ret == MBEDTLS_ERR_SSL_WANT_READ) ?
      CURLE_AGAIN : CURLE_RECV_ERROR;
    return -1;
  }

  return (ssize_t)ret;
}

void Curl_mbedtls_close_all(struct SessionHandle *data)
{
  (void)data;
}

void Curl_mbedtls_close(struct connectdata *conn, int sockindex)
{
  mbedtls_free_connection(&conn->ssl[sockindex]);
}

void Curl_mbedtls_session_free(void *ptr)
{
  free(ptr);
}

size_t Curl_mbedtls_version(char *buffer, size_t size)
{
  unsigned int version = mbedtls_version_get_number();
  return snprintf(buffer, size, "mbedTLS/%d.%d.%d",
                  (version >> 24) & 0xff,
                  (version >> 16) & 0xff,
                  (version >> 8) & 0xff);
}

static CURLcode
mbedtls_connect_common(struct connectdata *conn, int sockindex,
                        bool nonblocking, bool *done)
{
  CURLcode retcode;
  struct SessionHandle *data = conn->data;
  struct ssl_connect_data *connssl = &conn->ssl[sockindex];
  curl_socket_t sockfd = conn->sock[sockindex];
  long timeout_ms;
  int what;

  if(ssl_connection_complete == connssl->state) {
    *done = TRUE;
    return CURLE_OK;
  }

  if(ssl_connect_1 == connssl->connecting_state) {
    timeout_ms = Curl_timeleft(data, NULL, TRUE);
    if(timeout_ms < 0) {
      failf(data, "SSL connection timeout");
      mbedtls_free_connection(connssl);
      return CURLE_OPERATION_TIMEDOUT;
    }
    retcode = mbedtls_connect_step1(conn, sockindex);
    if(retcode)
      return retcode;
  }

  while(ssl_connect_2 == connssl->connecting_state ||
        ssl_connect_2_reading == connssl->connecting_state ||
        ssl_connect_2_writing == connssl->connecting_state) {

    timeout_ms = Curl_timeleft(data, NULL, TRUE);
    if(timeout_ms < 0) {
      failf(data, "SSL connection timeout");
      mbedtls_free_connection(connssl);
      return CURLE_OPERATION_TIMEDOUT;
    }

    if(connssl->connecting_state == ssl_connect_2_reading ||
       connssl->connecting_state == ssl_connect_2_writing) {

#ifndef _XBOX
      curl_socket_t writefd = ssl_connect_2_writing ==
        connssl->connecting_state ? sockfd : CURL_SOCKET_BAD;
      curl_socket_t readfd = ssl_connect_2_reading ==
        connssl->connecting_state ? sockfd : CURL_SOCKET_BAD;

      what = Curl_socket_ready(readfd, writefd,
                               nonblocking ? 0 : timeout_ms);
      if(what < 0) {
        failf(data, "select/poll on SSL socket, errno: %d", SOCKERRNO);
        mbedtls_free_connection(connssl);
        return CURLE_SSL_CONNECT_ERROR;
      }
      else if(0 == what) {
        if(nonblocking) {
          *done = FALSE;
          return CURLE_OK;
        }
        failf(data, "SSL connection timeout");
        mbedtls_free_connection(connssl);
        return CURLE_OPERATION_TIMEDOUT;
      }
#endif
    }

    retcode = mbedtls_connect_step2(conn, sockindex);
    if(retcode || (nonblocking &&
                   (ssl_connect_2 == connssl->connecting_state ||
                    ssl_connect_2_reading == connssl->connecting_state ||
                    ssl_connect_2_writing == connssl->connecting_state))) {
      if(!retcode && nonblocking) {
        *done = FALSE;
      }
      return retcode;
    }
  }

  if(ssl_connect_3 == connssl->connecting_state) {
    retcode = mbedtls_connect_step3(conn, sockindex);
    if(retcode)
      return retcode;
  }

  if(ssl_connect_done == connssl->connecting_state) {
    connssl->state = ssl_connection_complete;
    conn->recv[sockindex] = mbedtls_recv_func;
    conn->send[sockindex] = mbedtls_send_func;
    *done = TRUE;
  }
  else
    *done = FALSE;

  connssl->connecting_state = ssl_connect_1;
  return CURLE_OK;
}

CURLcode
Curl_mbedtls_connect_nonblocking(struct connectdata *conn,
                                  int sockindex, bool *done)
{
  return mbedtls_connect_common(conn, sockindex, TRUE, done);
}

CURLcode
Curl_mbedtls_connect(struct connectdata *conn, int sockindex)
{
  CURLcode retcode;
  bool done = FALSE;

  retcode = mbedtls_connect_common(conn, sockindex, FALSE, &done);
  if(retcode)
    return retcode;

  DEBUGASSERT(done);
  return CURLE_OK;
}

#endif /* USE_MBEDTLS */
