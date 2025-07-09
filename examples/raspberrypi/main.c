#include <arpa/inet.h>
#include <gst/gst.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "peer.h"

// const char CAMERA_PIPELINE[] = "libcamerasrc ! video/x-raw, format=(string)NV12, width=(int)1280, height=(int)960, framerate=(fraction)30/1, interlace-mode=(string)progressive, colorimetry=(string)bt709 ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! video/x-h264, stream-format=(string)byte-stream, level=(string)4, alighnment=(string)au ! h264parse config-interval=-1 ! appsink name=camera-sink";
const char CAMERA_PIPELINE[] = "filesrc location=palace.mp4 ! qtdemux name=demux demux.video_0 ! queue ! h264parse ! video/x-h264, stream-format=byte-stream, alignment=au ! h264parse config-interval=30 ! appsink name=camera-sink";
const char MIC_PIPELINE[] = "alsasrc latency-time=20000 device=plughw:seeed2micvoicec,0 ! audio/x-raw,format=S16LE,rate=8000,channels=1 ! alawenc ! appsink name=mic-sink";

const char SPK_PIPELINE[] = "appsrc name=spk-src format=time ! alawdec ! audio/x-raw,format=S16LE,rate=8000,channels=1 ! alsasink sync=false device=plughw:seeed2micvoicec,0";

int g_interrupted = 0;
PeerConnection* g_pc = NULL;
PeerConnectionState g_state;
int g_reconnect_attempt = 0;
time_t g_last_connected_time = 0;

typedef struct Media {
  // Camera elements
  GstElement* camera_pipeline;
  GstElement* camera_sink;

  // Microphone elements
  GstElement* mic_pipeline;
  GstElement* mic_sink;

  // Speaker elements
  GstElement* spk_pipeline;
  GstElement* spk_src;

} Media;

Media g_media;

static void cleanup_connection(void);
static int establish_connection(const char* url, const char* token);
static void handle_connection_state_change(PeerConnectionState state);

// 新增：连接状态变化处理
static void handle_connection_state_change(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_COMPLETED:
      printf("Connection established\n");
      g_reconnect_attempt = 0;
      g_last_connected_time = time(NULL);
      
      // 启动媒体流水线
      gst_element_set_state(g_media.camera_pipeline, GST_STATE_PLAYING);
      gst_element_set_state(g_media.mic_pipeline, GST_STATE_PLAYING);
      gst_element_set_state(g_media.spk_pipeline, GST_STATE_PLAYING);
      break;
      
    case PEER_CONNECTION_DISCONNECTED:
    case PEER_CONNECTION_FAILED:
    case PEER_CONNECTION_CLOSED:
      printf("Connection lost: %s\n", peer_connection_state_to_string(state));
      
      // 停止媒体流水线
      gst_element_set_state(g_media.camera_pipeline, GST_STATE_PAUSED);
      gst_element_set_state(g_media.mic_pipeline, GST_STATE_PAUSED);
      gst_element_set_state(g_media.spk_pipeline, GST_STATE_PAUSED);
      
      // 准备重连
      g_reconnect_attempt++;

      if (state == PEER_CONNECTION_CLOSED) {
        
      }
      break;
      
    default:
      break;
  }
}

static void onconnectionstatechange(PeerConnectionState state, void* data) {
  printf("State changed: %s\n", peer_connection_state_to_string(state));
  g_state = state;
  
  // 处理状态变化
  handle_connection_state_change(state);
}

static GstFlowReturn on_video_data(GstElement* sink, void* data) {
  GstSample* sample;
  GstBuffer* buffer;
  GstMapInfo info;

  g_signal_emit_by_name(sink, "pull-sample", &sample);

  if (sample) {
    buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &info, GST_MAP_READ);
    
    // 仅在连接状态下发送数据
    if (g_pc && g_state == PEER_CONNECTION_COMPLETED) {
      peer_connection_send_video(g_pc, info.data, info.size);
    }
    
    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}

static GstFlowReturn on_audio_data(GstElement* sink, void* data) {
  GstSample* sample;
  GstBuffer* buffer;
  GstMapInfo info;

  g_signal_emit_by_name(sink, "pull-sample", &sample);

  if (sample) {
    buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &info, GST_MAP_READ);
    
    // 仅在连接状态下发送数据
    if (g_pc && g_state == PEER_CONNECTION_COMPLETED) {
      peer_connection_send_audio(g_pc, info.data, info.size);
    }
    
    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}

// 新增：音频接收处理
static void on_audio_received(void* data, size_t size, void* user_data) {
  GstBuffer* buffer = gst_buffer_new_allocate(NULL, size, NULL);
  GstMapInfo map;
  
  gst_buffer_map(buffer, &map, GST_MAP_WRITE);
  memcpy(map.data, data, size);
  gst_buffer_unmap(buffer, &map);
  
  GstFlowReturn ret;
  g_signal_emit_by_name(g_media.spk_src, "push-buffer", buffer, &ret);
  
  gst_buffer_unref(buffer);
  
  if (ret != GST_FLOW_OK) {
    printf("Failed to push audio buffer: %d\n", ret);
  }
}

static void onopen(void* user_data) {
  printf("Data channel opened\n");
}

static void onclose(void* user_data) {
  printf("Data channel closed\n");
}

static void onmessasge(char* msg, size_t len, void* user_data, uint16_t sid) {
  printf("Received message: %.*s\n", (int)len, msg);

  if (len >= 4 && strncmp(msg, "ping", 4) == 0) {
    printf("Sending pong\n");
    peer_connection_datachannel_send(g_pc, "pong", 4);
  }
}

static void on_request_keyframe(void* data) {
  printf("Requesting keyframe\n");
}

static void signal_handler(int signal) {
  g_interrupted = 1;
}

static void* peer_singaling_task(void* data) {
  while (!g_interrupted) {
    peer_signaling_loop();
    usleep(1000);
  }

  pthread_exit(NULL);
}

static void* peer_connection_task(void* data) {
  while (!g_interrupted) {
    if (g_pc) {
      peer_connection_loop(g_pc);
    }
    usleep(1000);
  }

  pthread_exit(NULL);
}

// 新增：清理连接资源
static void cleanup_connection() {
  if (g_pc) {
    // 停止媒体流水线
    gst_element_set_state(g_media.camera_pipeline, GST_STATE_NULL);
    gst_element_set_state(g_media.mic_pipeline, GST_STATE_NULL);
    gst_element_set_state(g_media.spk_pipeline, GST_STATE_NULL);
    
    // 断开信令连接
    peer_signaling_disconnect();
    
    // 销毁对等连接
    peer_connection_destroy(g_pc);
    g_pc = NULL;
  }
}

// 新增：建立新连接
static int establish_connection(const char* url, const char* token) {
  cleanup_connection();
  
  PeerConfiguration config = {
      .ice_servers = {
          // {.urls = "stun:stun.l.google.com:19302"},
          {.urls = "stun:117.72.41.107:3478"},
          {.urls = "turn:117.72.41.107:3478", .username = "mh88", .credential = "1115"},
      },
      .datachannel = DATA_CHANNEL_STRING,
      .video_codec = CODEC_H264,
      .audio_codec = CODEC_PCMA,
      .on_request_keyframe = on_request_keyframe};
  
  // 创建新连接
  g_pc = peer_connection_create(&config);
  if (!g_pc) {
    printf("Failed to create peer connection\n");
    return -1;
  }
  
  // 设置回调
  peer_connection_oniceconnectionstatechange(g_pc, onconnectionstatechange);
  peer_connection_ondatachannel(g_pc, onmessasge, onopen, onclose);
  // peer_connection_set_audio_receive_callback(g_pc, on_audio_received, NULL);
  
  // 连接信令服务器
  if (peer_signaling_connect(url, token, g_pc) != 0) {
    printf("Failed to connect to signaling server\n");
    peer_connection_destroy(g_pc);
    g_pc = NULL;
    return -1;
  }
  
  return 0;
}

void print_usage(const char* prog_name) {
  printf("Usage: %s -u <url> [-t <token>]\n", prog_name);
}

void parse_arguments(int argc, char* argv[], const char** url, const char** token) {
  *token = NULL;
  *url = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 && (i + 1) < argc) {
      *url = argv[++i];
    } else if (strcmp(argv[i], "-t") == 0 && (i + 1) < argc) {
      *token = argv[++i];
    } else {
      print_usage(argv[0]);
      exit(1);
    }
  }

  if (*url == NULL) {
    print_usage(argv[0]);
    exit(1);
  }
}

int main(int argc, char* argv[]) {
  const char* url = NULL;
  const char* token = NULL;

  parse_arguments(argc, argv, &url, &token);

  printf("=========== Parsed Arguments ===========\n");
  printf(" %-5s : %s\n", "URL", url);
  printf(" %-5s : %s\n", "Token", token ? token : "");
  printf("========================================\n");

  pthread_t peer_singaling_thread;
  pthread_t peer_connection_thread;

  signal(SIGINT, signal_handler);

  gst_init(&argc, &argv);

  // 初始化媒体流水线
  g_media.camera_pipeline = gst_parse_launch(CAMERA_PIPELINE, NULL);
  g_media.camera_sink = gst_bin_get_by_name(GST_BIN(g_media.camera_pipeline), "camera-sink");
  g_signal_connect(g_media.camera_sink, "new-sample", G_CALLBACK(on_video_data), NULL);
  g_object_set(g_media.camera_sink, "emit-signals", TRUE, NULL);

  g_media.mic_pipeline = gst_parse_launch(MIC_PIPELINE, NULL);
  g_media.mic_sink = gst_bin_get_by_name(GST_BIN(g_media.mic_pipeline), "mic-sink");
  g_signal_connect(g_media.mic_sink, "new-sample", G_CALLBACK(on_audio_data), NULL);
  g_object_set(g_media.mic_sink, "emit-signals", TRUE, NULL);

  g_media.spk_pipeline = gst_parse_launch(SPK_PIPELINE, NULL);
  g_media.spk_src = gst_bin_get_by_name(GST_BIN(g_media.spk_pipeline), "spk-src");
  g_object_set(g_media.spk_src, "emit-signals", TRUE, NULL);

  // 初始化PeerConnection库
  peer_init();

  // 初始连接
  if (establish_connection(url, token) < 0) {
    printf("Initial connection failed\n");
    return 1;
  }

  // 创建线程
  pthread_create(&peer_connection_thread, NULL, peer_connection_task, NULL);
  pthread_create(&peer_singaling_thread, NULL, peer_singaling_task, NULL);

  // 主循环：监控连接状态并处理重连
  while (!g_interrupted) {
    // 检查是否需要重连
    // if (g_state == PEER_CONNECTION_DISCONNECTED || 
    //     g_state == PEER_CONNECTION_FAILED ||
    //     g_state == PEER_CONNECTION_CLOSED) {
      
    //   // 计算重连间隔（指数退避）
    //   int delay_seconds = 1 << (g_reconnect_attempt < 8 ? g_reconnect_attempt : 8);
      
    //   printf("Attempting reconnect in %d seconds (attempt %d)\n", 
    //          delay_seconds, g_reconnect_attempt);
      
    //   sleep(delay_seconds);
      
    //   // 尝试重新连接
    //   if (establish_connection(url, token) < 0) {
    //     printf("Reconnection attempt %d failed\n", g_reconnect_attempt);
    //   } else {
    //     printf("Reconnection attempt %d succeeded\n", g_reconnect_attempt);
    //   }
    // }
    
    // // 检查连接超时（可选）
    // if (g_state == PEER_CONNECTION_COMPLETED && 
    //     (time(NULL) - g_last_connected_time) > 300) {
    //   printf("Connection timeout, reconnecting...\n");
    //   g_state = PEER_CONNECTION_DISCONNECTED;
    // }
    
    sleep(1);
  }

  // 清理资源
  cleanup_connection();
  
  // 释放媒体资源
  gst_object_unref(g_media.camera_sink);
  gst_object_unref(g_media.camera_pipeline);
  gst_object_unref(g_media.mic_sink);
  gst_object_unref(g_media.mic_pipeline);
  gst_object_unref(g_media.spk_src);
  gst_object_unref(g_media.spk_pipeline);

  // 等待线程结束
  pthread_join(peer_singaling_thread, NULL);
  pthread_join(peer_connection_thread, NULL);

  // 释放PeerConnection库资源
  peer_deinit();

  return 0;
}