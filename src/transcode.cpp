#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include "glad.c"
}



// 队列模板类，用于缓存Packet和Frame
template<typename T>
class MediaQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool abort_request_ = false;

public:
    MediaQueue() = default;
    ~MediaQueue() {
        abort();
        clear();
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = true;
        cond_.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            auto item = queue_.front();
            queue_.pop();
            // 释放资源的逻辑，根据T类型实现
            if constexpr (std::is_same_v<T, AVPacket*>) {
                av_packet_free(&item);
            } else if constexpr (std::is_same_v<T, AVFrame*>) {
                av_frame_free(&item);
            }
        }
    }

    int push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (abort_request_) {
            return -1;
        }
        queue_.push(item);
        cond_.notify_one();
        return 0;
    }

    int pop(T &item, bool block = true) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (block) {
            cond_.wait(lock, [this] { return !queue_.empty() || abort_request_; });
        }
        if (abort_request_) {
            return -1;
        }
        if (queue_.empty()) {
            return 0;
        }
        item = queue_.front();
        queue_.pop();
        return 1;
    }

    int size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
};

class WatermarkRenderer;

// 处理上下文
struct ProcessingContext {
    // 输入
    AVFormatContext *input_format_ctx = nullptr;
    
    // 输出
    AVFormatContext *output_format_ctx = nullptr;
    std::string output_filename;
    
    // 流索引
    int video_stream_index = -1;
    int audio_stream_index = -1;
    
    // 解码器
    AVCodecContext *video_decoder_ctx = nullptr;
    AVCodecContext *audio_decoder_ctx = nullptr;
    
    // 编码器
    AVCodecContext *video_encoder_ctx = nullptr;
    AVCodecContext *audio_encoder_ctx = nullptr;
    
    // 队列
    MediaQueue<AVPacket*> video_packet_queue;    // 解码前的视频packet队列
    MediaQueue<AVPacket*> audio_packet_queue;    // 解码前的音频
    MediaQueue<AVFrame*> video_frame_queue;      // 解码后的视频帧队列
    MediaQueue<AVFrame*> audio_frame_queue;      // 解码后的音频帧队列
    MediaQueue<AVPacket*> video_encoded_packet_queue;  // 编码后的视频packet队列
    MediaQueue<AVPacket*> audio_encoded_packet_queue;  // 编码后的音频packet队列
    MediaQueue<AVFrame*> video_frames_for_render_queue;  // 待渲染的视频帧队列
    MediaQueue<AVFrame*> watermarked_frames_queue;  // 已添加水印的视频帧队列
    
    // 转换器
    struct SwsContext *sws_ctx = nullptr;        // 视频格式转换
    struct SwrContext *swr_ctx = nullptr;        // 音频格式转换
    
    // OpenGL相关
    std::string watermark_path;
    std::unique_ptr<WatermarkRenderer> watermark_renderer;
    std::mutex watermark_mutex;  // 确保OpenGL操作线程安全

    // 线程控制
    bool quit = false;
    std::thread demux_thread;
    std::thread video_decode_thread;
    std::thread audio_decode_thread;
    std::thread render_thread;
    std::thread video_encode_thread;
    std::thread audio_encode_thread;
    std::thread mux_thread;
};

class WatermarkRenderer {
private:
    GLFWwindow* window;
    GLuint fbo;          // 帧缓冲对象
    GLuint video_tex;    // 视频纹理
    GLuint watermark_tex;// 水印纹理
    int width, height;   // 视频帧尺寸

    // 着色器程序和缓冲对象
    GLuint shader_program;
    GLuint VAO, VBO, EBO;

    // 顶点数据 - 视频帧和水印位置(右下角)
    float vertices[40] = {
        // 视频帧顶点 (全屏)
        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,  // 左上
         1.0f,  1.0f, 0.0f,  1.0f, 1.0f,  // 右上
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,  // 右下
        -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,  // 左下
        // 水印顶点 (右下角)
         0.5f, -0.5f, 0.0f,  0.0f, 1.0f,  // 左上
         1.0f, -0.5f, 0.0f,  1.0f, 1.0f,  // 右上
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,  // 右下
         0.5f, -1.0f, 0.0f,  0.0f, 0.0f   // 左下
    };

    unsigned int indices[12] = {
        0, 1, 2,   // 视频帧三角形1
        2, 3, 0,   // 视频帧三角形2
        4, 5, 6,   // 水印三角形1
        6, 7, 4    // 水印三角形2
    };

    // 编译着色器
    GLuint compile_shader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, NULL);
        glCompileShader(shader);
        
        int success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(shader, 512, NULL, infoLog);
            std::cerr << "着色器编译失败: " << infoLog << std::endl;
        }
        return shader;
    }

    // 初始化着色器
    void init_shaders() {
        const char* vertexShaderSource = "#version 330 core\n"
            "layout (location = 0) in vec3 aPos;\n"
            "layout (location = 1) in vec2 aTexCoord;\n"
            "out vec2 TexCoord;\n"
            "void main()\n"
            "{\n"
            "   gl_Position = vec4(aPos, 1.0);\n"
            "   TexCoord = aTexCoord;\n"
            "}\0";

        const char* fragmentShaderSource = "#version 330 core\n"
            "out vec4 FragColor;\n"
            "in vec2 TexCoord;\n"
            "uniform sampler2D videoTexture;\n"
            "uniform sampler2D watermarkTexture;\n"
            "uniform int isWatermark;\n"
            "void main()\n"
            "{\n"
            "   if(isWatermark == 0) {\n"
            "       FragColor = texture(videoTexture, TexCoord);\n"
            "   } else {\n"
            "       vec4 watermark = texture(watermarkTexture, TexCoord);\n"
            "       FragColor = watermark.a > 0.1 ? watermark : vec4(0.0);\n"
            "   }\n"
            "}\0";

        GLuint vertexShader = compile_shader(GL_VERTEX_SHADER, vertexShaderSource);
        GLuint fragmentShader = compile_shader(GL_FRAGMENT_SHADER, fragmentShaderSource);

        shader_program = glCreateProgram();
        glAttachShader(shader_program, vertexShader);
        glAttachShader(shader_program, fragmentShader);
        glLinkProgram(shader_program);

        int success;
        glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(shader_program, 512, NULL, infoLog);
            std::cerr << "着色器程序链接失败: " << infoLog << std::endl;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
    }

    // 初始化缓冲对象
    void init_buffers() {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);

        glBindVertexArray(VAO);

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        // 位置属性
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        // 纹理坐标属性
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);
    }

    // 初始化帧缓冲和纹理
    void init_framebuffer() {
        // 创建视频帧纹理
        glGenTextures(1, &video_tex);
        glBindTexture(GL_TEXTURE_2D, video_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

        // 创建帧缓冲
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, video_tex, 0);
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "帧缓冲不完整！" << std::endl;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // 加载水印图片
    bool load_watermark_image(const std::string& path) {
        int w, h, nrChannels;
        unsigned char *data = stbi_load(path.c_str(), &w, &h, &nrChannels, 0);
        if (!data) {
            std::cerr << "无法加载水印图片: " << path << std::endl;
            return false;
        }

        glGenTextures(1, &watermark_tex);
        glBindTexture(GL_TEXTURE_2D, watermark_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        stbi_image_free(data);
        return true;
    }

public:
    WatermarkRenderer(int w, int h, const std::string& watermark_path) 
        : width(w), height(h), window(nullptr), fbo(0), video_tex(0), watermark_tex(0),
          shader_program(0), VAO(0), VBO(0), EBO(0) {
        // 初始化GLFW
        if (!glfwInit()) {
            std::cerr << "无法初始化GLFW" << std::endl;
            return;
        }

        // 创建不可见窗口用于OpenGL上下文
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(width, height, "Watermark Renderer", NULL, NULL);
        if (!window) {
            std::cerr << "无法创建GLFW窗口" << std::endl;
            glfwTerminate();
            return;
        }

        // 绑定上下文
        glfwMakeContextCurrent(window);

        // 初始化GLAD
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            std::cerr << "无法初始化GLAD" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            return;
        }

        // 初始化组件
        init_shaders();
        init_buffers();
        init_framebuffer();
        
        // 加载水印
        if (!load_watermark_image(watermark_path)) {
            cleanup();
        }
    }

    ~WatermarkRenderer() {
        cleanup();
    }

    void cleanup() {
        if (window) {
            glDeleteVertexArrays(1, &VAO);
            glDeleteBuffers(1, &VBO);
            glDeleteBuffers(1, &EBO);
            glDeleteProgram(shader_program);
            glDeleteTextures(1, &video_tex);
            glDeleteTextures(1, &watermark_tex);
            glDeleteFramebuffers(1, &fbo);
            glfwDestroyWindow(window);
            glfwTerminate();
            window = nullptr;
        }
    }

    bool is_valid() const {
        return window != nullptr;
    }

    // 给视频帧添加水印
    int add_watermark(AVFrame* input_frame, AVFrame* output_frame) {
        if (!is_valid() || !input_frame || !output_frame) {
            return -1;
        }

        // 确保输出帧已分配
        output_frame->width = width;
        output_frame->height = height;
        output_frame->format = input_frame->format;
        int ret = av_frame_get_buffer(output_frame, 0);
        if (ret < 0) {
            std::cerr << "无法为输出帧分配缓冲区" << std::endl;
            return ret;
        }

        // 将YUV转换为RGB供OpenGL使用
        AVFrame* rgb_frame = av_frame_alloc();
        rgb_frame->width = width;
        rgb_frame->height = height;
        rgb_frame->format = AV_PIX_FMT_RGB24;
        ret = av_frame_get_buffer(rgb_frame, 0);
        if (ret < 0) {
            std::cerr << "无法为RGB帧分配缓冲区" << std::endl;
            av_frame_free(&rgb_frame);
            return ret;
        }

        // 转换格式
        SwsContext* sws_ctx = sws_getContext(
            width, height, (AVPixelFormat)input_frame->format,
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!sws_ctx) {
            std::cerr << "无法创建SwsContext" << std::endl;
            av_frame_free(&rgb_frame);
            return -1;
        }

        sws_scale(sws_ctx, input_frame->data, input_frame->linesize, 0, height,
                 rgb_frame->data, rgb_frame->linesize);
        sws_freeContext(sws_ctx);

        // 上传视频帧数据到纹理
        glBindTexture(GL_TEXTURE_2D, video_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, 
                       GL_RGB, GL_UNSIGNED_BYTE, rgb_frame->data[0]);

        // 渲染到帧缓冲
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(shader_program);

        // 绘制视频帧
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, video_tex);
        glUniform1i(glGetUniformLocation(shader_program, "videoTexture"), 0);
        glUniform1i(glGetUniformLocation(shader_program, "isWatermark"), 0);
        
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);  // 绘制视频帧

        // 绘制水印
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, watermark_tex);
        glUniform1i(glGetUniformLocation(shader_program, "watermarkTexture"), 1);
        glUniform1i(glGetUniformLocation(shader_program, "isWatermark"), 1);
        
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(6 * sizeof(unsigned int)));  // 绘制水印

        // 从帧缓冲读取数据
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame->data[0]);

        // 将RGB转换回YUV
        sws_ctx = sws_getContext(
            width, height, AV_PIX_FMT_RGB24,
            width, height, (AVPixelFormat)input_frame->format,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!sws_ctx) {
            std::cerr << "无法创建SwsContext" << std::endl;
            av_frame_free(&rgb_frame);
            return -1;
        }

        sws_scale(sws_ctx, rgb_frame->data, rgb_frame->linesize, 0, height,
                 output_frame->data, output_frame->linesize);
        sws_freeContext(sws_ctx);

        // 复制时间戳等信息
        output_frame->pts = input_frame->pts;
        output_frame->pkt_dts = input_frame->pkt_dts;
        output_frame->duration = input_frame->duration;

        av_frame_free(&rgb_frame);
        return 0;
    }
};
// 初始化输入
int init_input(ProcessingContext *ctx, const std::string &input_filename) {
    // 打开输入文件
    int ret = avformat_open_input(&ctx->input_format_ctx, input_filename.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "无法打开输入文件: " << input_filename << std::endl;
        return ret;
    }

    // 获取流信息
    ret = avformat_find_stream_info(ctx->input_format_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "无法获取流信息" << std::endl;
        return ret;
    }

    // 查找视频流和音频流
    for (unsigned int i = 0; i < ctx->input_format_ctx->nb_streams; i++) {
        if (ctx->input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
            ctx->video_stream_index == -1) {
            ctx->video_stream_index = i;
        } else if (ctx->input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && 
                   ctx->audio_stream_index == -1) {
            ctx->audio_stream_index = i;
        }
    }

    if (ctx->video_stream_index == -1) {
        std::cerr << "未找到视频流" << std::endl;
        return -1;
    }

    // 初始化视频解码器
    const AVCodec *video_decoder = avcodec_find_decoder(
        ctx->input_format_ctx->streams[ctx->video_stream_index]->codecpar->codec_id);
    if (!video_decoder) {
        std::cerr << "找不到视频解码器" << std::endl;
        return -1;
    }

    ctx->video_decoder_ctx = avcodec_alloc_context3(video_decoder);
    if (!ctx->video_decoder_ctx) {
        std::cerr << "无法分配视频解码器上下文" << std::endl;
        return -1;
    }

    ret = avcodec_parameters_to_context(ctx->video_decoder_ctx, 
        ctx->input_format_ctx->streams[ctx->video_stream_index]->codecpar);
    if (ret < 0) {
        std::cerr << "无法将流参数复制到解码器上下文" << std::endl;
        return ret;
    }

    ret = avcodec_open2(ctx->video_decoder_ctx, video_decoder, nullptr);
    if (ret < 0) {
        std::cerr << "无法打开视频解码器" << std::endl;
        return ret;
    }

    // 初始化音频解码器（如果存在音频流）
    if (ctx->audio_stream_index != -1) {
        const AVCodec *audio_decoder = avcodec_find_decoder(
            ctx->input_format_ctx->streams[ctx->audio_stream_index]->codecpar->codec_id);
        if (!audio_decoder) {
            std::cerr << "找不到音频解码器" << std::endl;
            return -1;
        }

        ctx->audio_decoder_ctx = avcodec_alloc_context3(audio_decoder);
        if (!ctx->audio_decoder_ctx) {
            std::cerr << "无法分配音频解码器上下文" << std::endl;
            return -1;
        }

        ret = avcodec_parameters_to_context(ctx->audio_decoder_ctx, 
            ctx->input_format_ctx->streams[ctx->audio_stream_index]->codecpar);
        if (ret < 0) {
            std::cerr << "无法将流参数复制到音频解码器上下文" << std::endl;
            return ret;
        }

        ret = avcodec_open2(ctx->audio_decoder_ctx, audio_decoder, nullptr);
        if (ret < 0) {
            std::cerr << "无法打开音频解码器" << std::endl;
            return ret;
        }
    }

    // OpenGL用于水印渲染的初始化在video_decode_thread_func中完成

    return 0;
}

// 初始化输出
int init_output(ProcessingContext *ctx) {
    // 创建输出格式上下文
    int ret = avformat_alloc_output_context2(&ctx->output_format_ctx, nullptr, "avi", ctx->output_filename.c_str());
    if (!ctx->output_format_ctx) {
        std::cerr << "无法创建输出格式上下文" << std::endl;
        return -1;
    }

    // 添加视频流
    const AVCodec *video_encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!video_encoder) {
        std::cerr << "找不到MPEG4编码器" << std::endl;
        return -1;
    }

    AVStream *video_stream = avformat_new_stream(ctx->output_format_ctx, nullptr);
    if (!video_stream) {
        std::cerr << "无法创建视频流" << std::endl;
        return -1;
    }

    ctx->video_encoder_ctx = avcodec_alloc_context3(video_encoder);
    if (!ctx->video_encoder_ctx) {
        std::cerr << "无法分配视频编码器上下文" << std::endl;
        return -1;
    }

    // 配置视频编码器为YUV420
    ctx->video_encoder_ctx->codec_id = video_encoder->id;
    ctx->video_encoder_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->video_encoder_ctx->width = ctx->video_decoder_ctx->width;
    ctx->video_encoder_ctx->height = ctx->video_decoder_ctx->height;
    ctx->video_encoder_ctx->time_base = {1, 25};  // 25fps
    ctx->video_encoder_ctx->framerate = {25, 1};
    ctx->video_encoder_ctx->bit_rate = 400000;  // 400kbps
    ctx->video_encoder_ctx->gop_size = 10;  // 每10帧一个关键帧
    
    if (ctx->output_format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->video_encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(ctx->video_encoder_ctx, video_encoder, nullptr);
    if (ret < 0) {
        std::cerr << "无法打开视频编码器" << std::endl;
        return ret;
    }

    ret = avcodec_parameters_from_context(video_stream->codecpar, ctx->video_encoder_ctx);
    if (ret < 0) {
        std::cerr << "无法将编码器参数复制到流" << std::endl;
        return ret;
    }
    video_stream->time_base = ctx->video_encoder_ctx->time_base;

    // 添加音频流（如果有）
    if (ctx->audio_stream_index != -1) {
        const AVCodec *audio_encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!audio_encoder) {
            std::cerr << "找不到AAC编码器" << std::endl;
            return -1;
        }

        AVStream *audio_stream = avformat_new_stream(ctx->output_format_ctx, nullptr);
        if (!audio_stream) {
            std::cerr << "无法创建音频流" << std::endl;
            return -1;
        }

        ctx->audio_encoder_ctx = avcodec_alloc_context3(audio_encoder);
        if (!ctx->audio_encoder_ctx) {
            std::cerr << "无法分配音频编码器上下文" << std::endl;
            return -1;
        }

        // 配置音频编码器
        ctx->audio_encoder_ctx->codec_id = audio_encoder->id;
        ctx->audio_encoder_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
        ctx->audio_encoder_ctx->sample_fmt = audio_encoder->sample_fmts ? 
            audio_encoder->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        ctx->audio_encoder_ctx->bit_rate = 128000;  // 128kbps
        ctx->audio_encoder_ctx->sample_rate = 44100;  // 44.1kHz
        ctx->audio_encoder_ctx->channels = 2;  // 立体声
        ctx->audio_encoder_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
        ctx->audio_encoder_ctx->frame_size = 1024;  // 每帧1024采样点
        
        if (ctx->output_format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            ctx->audio_encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        ret = avcodec_open2(ctx->audio_encoder_ctx, audio_encoder, nullptr);
        if (ret < 0) {
            std::cerr << "无法打开音频编码器" << std::endl;
            return ret;
        }

        ret = avcodec_parameters_from_context(audio_stream->codecpar, ctx->audio_encoder_ctx);
        if (ret < 0) {
            std::cerr << "无法将音频编码器参数复制到流" << std::endl;
            return ret;
        }
        audio_stream->time_base = {1, ctx->audio_encoder_ctx->sample_rate};
    }

    // 打开输出文件
    if (!(ctx->output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ctx->output_format_ctx->pb, ctx->output_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::cerr << "无法打开输出文件: " << ctx->output_filename << std::endl;
            return ret;
        }
    }

    // 写文件头
    ret = avformat_write_header(ctx->output_format_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "无法写文件头" << std::endl;
        return ret;
    }

    // 初始化视频转换器（转为YUV420）
    ctx->sws_ctx = sws_getContext(
        ctx->video_decoder_ctx->width, ctx->video_decoder_ctx->height,
        ctx->video_decoder_ctx->pix_fmt,
        ctx->video_encoder_ctx->width, ctx->video_encoder_ctx->height,
        ctx->video_encoder_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!ctx->sws_ctx) {
        std::cerr << "无法初始化视频转换器" << std::endl;
        return -1;
    }

    // 初始化音频转换器（如果需要）
    if (ctx->audio_stream_index != -1 && 
        (ctx->audio_decoder_ctx->sample_fmt != ctx->audio_encoder_ctx->sample_fmt ||
         ctx->audio_decoder_ctx->sample_rate != ctx->audio_encoder_ctx->sample_rate ||
         ctx->audio_decoder_ctx->channel_layout != ctx->audio_encoder_ctx->channel_layout)) {
        
        ctx->swr_ctx = swr_alloc_set_opts(
            nullptr,
            ctx->audio_encoder_ctx->channel_layout, ctx->audio_encoder_ctx->sample_fmt,
            ctx->audio_encoder_ctx->sample_rate,
            ctx->audio_decoder_ctx->channel_layout, ctx->audio_decoder_ctx->sample_fmt,
            ctx->audio_decoder_ctx->sample_rate,
            0, nullptr
        );
        if (!ctx->swr_ctx || swr_init(ctx->swr_ctx) < 0) {
            std::cerr << "无法初始化音频转换器" << std::endl;
            return -1;
        }
    }

    return 0;
}

// Demuxer线程：读取输入文件，将Packet放入队列
void demux_thread_func(ProcessingContext *ctx) {
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "无法分配AVPacket" << std::endl;
        return;
    }

    while (!ctx->quit) {
        int ret = av_read_frame(ctx->input_format_ctx, pkt);
        if (ret < 0) {
            // 读取完毕或出错
            break;
        }

        // 将Packet放入队列（复制一份，因为av_read_frame返回的pkt会被重用）
        AVPacket *queue_pkt = av_packet_alloc();
        if (!queue_pkt || av_packet_ref(queue_pkt, pkt) < 0) {
            std::cerr << "无法复制AVPacket" << std::endl;
            av_packet_free(&queue_pkt);
        } else {
            if (pkt->stream_index == ctx->video_stream_index) {
                ctx->video_packet_queue.push(queue_pkt);
            } else if (pkt->stream_index == ctx->audio_stream_index) {
                ctx->audio_packet_queue.push(queue_pkt);
            } else {
                // 非视频非音频流，直接丢弃
                av_packet_free(&queue_pkt);
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    // 标记队列结束
    ctx->video_packet_queue.push(nullptr);
    if (ctx->audio_stream_index != -1) {
        ctx->audio_packet_queue.push(nullptr);
    }
}

// 视频解码线程：从队列取Packet，解码为Frame，放入视频Frame队列
void video_decode_thread_func(ProcessingContext *ctx) {
    AVPacket *pkt = nullptr;
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "无法分配AVFrame" << std::endl;
        return;
    }

    while (!ctx->quit) {
        int ret = ctx->video_packet_queue.pop(pkt);
        if (ret <= 0) {
            if (ret < 0) break;
            continue;
        }

        if (!pkt) {
            ctx->video_frame_queue.push(nullptr);
            break;
        }

        if (pkt->stream_index == ctx->video_stream_index) {
            ret = avcodec_send_packet(ctx->video_decoder_ctx, pkt);
            if (ret < 0) {
                std::cerr << "发送视频Packet到解码器失败" << std::endl;
            } else {
                while (ret >= 0 && !ctx->quit) {
                    ret = avcodec_receive_frame(ctx->video_decoder_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        std::cerr << "接收视频Frame失败" << std::endl;
                        break;
                    }

                    AVFrame *queue_frame = av_frame_alloc();
                    if (!queue_frame || av_frame_ref(queue_frame, frame) < 0) {
                        std::cerr << "无法复制视频Frame" << std::endl;
                        av_frame_free(&queue_frame);
                    } else {
                        std::cout<<"解码出视频帧，PTS="<<queue_frame->pts<<std::endl;
                        ctx->video_frames_for_render_queue.push(queue_frame);
                    }
                }
            }
        }
        av_packet_free(&pkt);
    }
    ctx->video_frames_for_render_queue.push(nullptr); // 标记视频解码结束
    av_frame_free(&frame);
}

// 音频解码线程：从队列取Packet，解码为Frame，放入音频Frame队列
void audio_decode_thread_func(ProcessingContext *ctx) {
    if (ctx->audio_stream_index == -1) return;

    AVPacket *pkt = nullptr;
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "无法分配AVFrame" << std::endl;
        return;
    }

    while (!ctx->quit) {
        int ret = ctx->audio_packet_queue.pop(pkt);
        if (ret <= 0) {
            if (ret < 0) break; // 队列已中止
            continue;
        }

        // 如果是nullptr，说明队列结束
        if (!pkt) {
            // 放入nullptr标记音频解码结束
            ctx->audio_frame_queue.push(nullptr);
            break;
        }

        // 只处理音频流
        if (pkt->stream_index == ctx->audio_stream_index) {
            // 发送Packet到解码器
            ret = avcodec_send_packet(ctx->audio_decoder_ctx, pkt);
            if (ret < 0) {
                std::cerr << "发送音频Packet到解码器失败" << std::endl;
            } else {
                // 接收解码后的Frame
                while (ret >= 0 && !ctx->quit) {
                    ret = avcodec_receive_frame(ctx->audio_decoder_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        std::cerr << "接收音频Frame失败" << std::endl;
                        break;
                    }

                    // 复制Frame并放入队列
                    AVFrame *queue_frame = av_frame_alloc();
                    if (!queue_frame || av_frame_ref(queue_frame, frame) < 0) {
                        std::cerr << "无法复制音频Frame" << std::endl;
                        av_frame_free(&queue_frame);
                    } else {
                        ctx->audio_frame_queue.push(queue_frame);
                    }
                }
            }
        }
        av_packet_free(&pkt);
    }

    av_frame_free(&frame);
}

void render_thread_func(ProcessingContext* ctx) {
    // 1. 在此线程中初始化WatermarkRenderer和OpenGL
    ctx->watermark_renderer = std::make_unique<WatermarkRenderer>(
        ctx->video_decoder_ctx->width, 
        ctx->video_decoder_ctx->height,
        ctx->watermark_path
    );
    if (!ctx->watermark_renderer->is_valid()) {
        std::cerr << "水印渲染器初始化失败" << std::endl;
        ctx->watermark_renderer.reset();
        return;
    }

    AVFrame* input_frame = nullptr;
    while (!ctx->quit) {
        // 2. 从解码线程的队列中取出帧
        std::cout<<"等待渲染帧..."<<std::endl;
        int ret = ctx->video_frames_for_render_queue.pop(input_frame);
        if (ret <= 0) {
            if (ret < 0) break;
            continue;
        }
        if (!input_frame) {
            // 结束标记
            ctx->watermarked_frames_queue.push(nullptr);
            break;
        }

        // 3. 执行OpenGL渲染
        AVFrame* watermarked_frame = av_frame_alloc();
        if (watermarked_frame && ctx->watermark_renderer->add_watermark(input_frame, watermarked_frame) == 0) {
            // 4. 将加水印后的帧推入新队列
            ctx->watermarked_frames_queue.push(watermarked_frame);
        } else {
            // 渲染失败，推入空帧或错误处理
            av_frame_free(&watermarked_frame);
        }
        av_frame_free(&input_frame);
    }
}

// 视频编码线程：从视频Frame队列取Frame，编码为Packet，放入编码后队列
void video_encode_thread_func(ProcessingContext *ctx) {
    AVFrame *frame = nullptr;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "无法分配AVPacket" << std::endl;
        return;
    }

    // 创建用于编码的Frame（YUV420格式）
    AVFrame *enc_frame = av_frame_alloc();
    enc_frame->format = ctx->video_encoder_ctx->pix_fmt;
    enc_frame->width = ctx->video_encoder_ctx->width;
    enc_frame->height = ctx->video_encoder_ctx->height;
    int ret = av_frame_get_buffer(enc_frame, 0);
    if (ret < 0) {
        std::cerr << "无法为编码Frame分配缓冲区" << std::endl;
        av_frame_free(&enc_frame);
        av_packet_free(&pkt);
        return;
    }
    int64_t frame_count = 0;

    while (!ctx->quit) {
        ret = ctx->watermarked_frames_queue.pop(frame);
        if (ret <= 0) {
            if (ret < 0) break; // 队列已中止
            continue;
        }

        // 如果是nullptr，说明队列结束
        if (!frame) {
            // 刷新编码器
            ret = avcodec_send_frame(ctx->video_encoder_ctx, nullptr);
            while (ret >= 0 && !ctx->quit) {
                ret = avcodec_receive_packet(ctx->video_encoder_ctx, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "视频编码失败" << std::endl;
                    break;
                }

                // 设置流索引
                pkt->stream_index = 0; // 视频流在输出中通常是第一个流
                
                // 转换时间基
                av_packet_rescale_ts(pkt, 
                                    ctx->video_encoder_ctx->time_base,
                                    ctx->output_format_ctx->streams[0]->time_base);

                // 复制Packet并放入队列
                AVPacket *queue_pkt = av_packet_alloc();
                if (!queue_pkt || av_packet_ref(queue_pkt, pkt) < 0) {
                    std::cerr << "无法复制编码后的视频Packet" << std::endl;
                    av_packet_free(&queue_pkt);
                } else {
                    ctx->video_encoded_packet_queue.push(queue_pkt);
                }
                
                av_packet_unref(pkt);
            }
            
            // 标记视频编码结束
            ctx->video_encoded_packet_queue.push(nullptr);
            av_frame_free(&frame);
            break;
        }

        // 转换为YUV420格式
        sws_scale(ctx->sws_ctx,
                 frame->data, frame->linesize, 0, frame->height,
                 enc_frame->data, enc_frame->linesize);

        // 设置时间戳
        enc_frame->pts = frame_count++;  // 从0开始递增
        // 转换为编码器时间基（如25fps对应time_base={1,25}）
        enc_frame->pts = av_rescale_q(enc_frame->pts, {1, 25}, ctx->video_encoder_ctx->time_base);
        
        // 发送Frame到编码器
        ret = avcodec_send_frame(ctx->video_encoder_ctx, enc_frame);
        if (ret < 0) {
            std::cerr << "发送视频Frame到编码器失败" << std::endl;
        } else {
            // 接收编码后的Packet
            while (ret >= 0 && !ctx->quit) {
                ret = avcodec_receive_packet(ctx->video_encoder_ctx, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "接收编码后的视频Packet失败" << std::endl;
                    break;
                }

                // 设置流索引
                pkt->stream_index = 0; // 视频流在输出中通常是第一个流
                
                // 转换时间基
                av_packet_rescale_ts(pkt, 
                                    ctx->video_encoder_ctx->time_base,
                                    ctx->output_format_ctx->streams[0]->time_base);

                // 复制Packet并放入队列
                AVPacket *queue_pkt = av_packet_alloc();
                if (!queue_pkt || av_packet_ref(queue_pkt, pkt) < 0) {
                    std::cerr << "无法复制编码后的视频Packet" << std::endl;
                    av_packet_free(&queue_pkt);
                } else {
                    ctx->video_encoded_packet_queue.push(queue_pkt);
                }
                
                av_packet_unref(pkt);
            }
        }

        av_frame_free(&frame);
    }

    ctx->video_encoded_packet_queue.push(nullptr); // 标记视频编码结束
    av_frame_free(&enc_frame);
    av_packet_free(&pkt);
}

// 音频编码线程：从音频Frame队列取Frame，编码为Packet，放入编码后队列
void audio_encode_thread_func(ProcessingContext *ctx) {
    if (ctx->audio_stream_index == -1 || !ctx->audio_encoder_ctx) return;

    AVFrame *frame = nullptr;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "无法分配AVPacket" << std::endl;
        return;
    }

    // 创建用于重采样的缓冲区
    uint8_t **swr_buf = nullptr;
    int swr_buf_size = 0;
    AVFrame *enc_frame = av_frame_alloc();
    enc_frame->format = ctx->audio_encoder_ctx->sample_fmt;
    enc_frame->channel_layout = ctx->audio_encoder_ctx->channel_layout;
    enc_frame->channels = ctx->audio_encoder_ctx->channels;
    enc_frame->sample_rate = ctx->audio_encoder_ctx->sample_rate;

    int64_t frame_count = 0;
    while (!ctx->quit) {
        int ret = ctx->audio_frame_queue.pop(frame);
        if (ret <= 0) {
            if (ret < 0) break; // 队列已中止
            continue;
        }

        // 如果是nullptr，说明队列结束
        if (!frame) {
            // 刷新编码器
            ret = avcodec_send_frame(ctx->audio_encoder_ctx, nullptr);
            while (ret >= 0 && !ctx->quit) {
                ret = avcodec_receive_packet(ctx->audio_encoder_ctx, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "音频编码失败" << std::endl;
                    break;
                }

                // 设置流索引（音频流在输出中通常是第二个流）
                pkt->stream_index = 1;
                
                // 转换时间基
                av_packet_rescale_ts(pkt, 
                                    ctx->audio_encoder_ctx->time_base,
                                    ctx->output_format_ctx->streams[1]->time_base);

                // 复制Packet并放入队列
                AVPacket *queue_pkt = av_packet_alloc();
                if (!queue_pkt || av_packet_ref(queue_pkt, pkt) < 0) {
                    std::cerr << "无法复制编码后的音频Packet" << std::endl;
                    av_packet_free(&queue_pkt);
                } else {
                    ctx->audio_encoded_packet_queue.push(queue_pkt);
                }
                
                av_packet_unref(pkt);
            }
            
            av_frame_free(&frame);
            break;
        }

        // 音频重采样（如果需要）
        if (ctx->swr_ctx) {
            // 计算需要的输出样本数
            int dst_nb_samples = av_rescale_rnd(
                swr_get_delay(ctx->swr_ctx, frame->sample_rate) + frame->nb_samples,
                ctx->audio_encoder_ctx->sample_rate, frame->sample_rate, AV_ROUND_UP);
            
            // 确保缓冲区足够大
            if (dst_nb_samples > swr_buf_size) {
                av_freep(&swr_buf[0]);
                ret = av_samples_alloc_array_and_samples(
                    &swr_buf, nullptr, ctx->audio_encoder_ctx->channels,
                    dst_nb_samples, ctx->audio_encoder_ctx->sample_fmt, 0);
                if (ret < 0) {
                    std::cerr << "无法分配音频重采样缓冲区" << std::endl;
                    av_frame_free(&frame);
                    continue;
                }
                swr_buf_size = dst_nb_samples;
            }
            
            // 执行重采样
            int dst_samples = swr_convert(ctx->swr_ctx, swr_buf, dst_nb_samples,
                                         (const uint8_t**)frame->data, frame->nb_samples);
            if (dst_samples < 0) {
                std::cerr << "音频重采样失败" << std::endl;
                av_frame_free(&frame);
                continue;
            }
            
            // 设置编码Frame的参数
            enc_frame->nb_samples = dst_samples;
            ret = av_frame_get_buffer(enc_frame, 0);
            if (ret < 0) {
                std::cerr << "无法为音频编码Frame分配缓冲区" << std::endl;
                av_frame_free(&frame);
                continue;
            }
            
            // 复制重采样后的数据
            memcpy(enc_frame->data[0], swr_buf[0], 
                   av_samples_get_buffer_size(nullptr, ctx->audio_encoder_ctx->channels,
                                             dst_samples, ctx->audio_encoder_ctx->sample_fmt, 1));
        } else {
            // 无需重采样，直接复制
            ret = av_frame_ref(enc_frame, frame);
            if (ret < 0) {
                std::cerr << "无法复制音频Frame" << std::endl;
                av_frame_free(&frame);
                continue;
            }
        }

        // 设置时间戳
        enc_frame->pts = frame_count++;
        
        // 发送Frame到编码器
        ret = avcodec_send_frame(ctx->audio_encoder_ctx, enc_frame);
        if (ret < 0) {
            std::cerr << "发送音频Frame到编码器失败" << std::endl;
        } else {
            // 接收编码后的Packet
            while (ret >= 0 && !ctx->quit) {
                ret = avcodec_receive_packet(ctx->audio_encoder_ctx, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "接收编码后的音频Packet失败" << std::endl;
                    break;
                }

                // 设置流索引（音频流在输出中通常是第二个流）
                pkt->stream_index = 1;
                
                // 转换时间基
                av_packet_rescale_ts(pkt, 
                                    ctx->audio_encoder_ctx->time_base,
                                    ctx->output_format_ctx->streams[1]->time_base);

                // 复制Packet并放入队列
                AVPacket *queue_pkt = av_packet_alloc();
                if (!queue_pkt || av_packet_ref(queue_pkt, pkt) < 0) {
                    std::cerr << "无法复制编码后的音频Packet" << std::endl;
                    av_packet_free(&queue_pkt);
                } else {
                    ctx->audio_encoded_packet_queue.push(queue_pkt);
                }
                
                av_packet_unref(pkt);
            }
        }

        av_frame_unref(enc_frame);
        av_frame_free(&frame);
    }

    // 清理
    if (swr_buf) {
        av_freep(&swr_buf[0]);
        av_freep(&swr_buf);
    }
    av_frame_free(&enc_frame);
    av_packet_free(&pkt);
}

// Muxer线程：从编码后队列取Packet，写入输出文件
void mux_thread_func(ProcessingContext *ctx) {
    AVPacket *video_pkt = nullptr;
    AVPacket *audio_pkt = nullptr;
    int video_done = 0;
    int audio_done = 0;

    while (!ctx->quit) {
        // 1. 非阻塞从视频队列取Packet（如果未结束）
        if (!video_done) {
            int ret = ctx->video_encoded_packet_queue.pop(video_pkt, false);  // 非阻塞
            if (ret == 1) {  // 取到数据
                if (!video_pkt) {  // 视频结束标记（nullptr）
                    video_done = 1;
                } else {
                    // 处理视频Packet：时间基转换 + 写入
                    AVStream *out_stream = ctx->output_format_ctx->streams[0];
                    av_packet_rescale_ts(video_pkt, 
                                        ctx->video_encoder_ctx->time_base, 
                                        out_stream->time_base);
                    video_pkt->pos = -1;

                    if (av_interleaved_write_frame(ctx->output_format_ctx, video_pkt) < 0) {
                        std::cerr << "写入视频Packet失败" << std::endl;
                    }
                    av_packet_free(&video_pkt);
                }
            } else if (ret < 0) {  // 队列异常中止
                video_done = 1;
            }
        }

        // 2. 非阻塞从音频队列取Packet（如果未结束且存在音频流）
        if (!audio_done && ctx->audio_stream_index != -1) {
            int ret = ctx->audio_encoded_packet_queue.pop(audio_pkt, false);  // 非阻塞
            if (ret == 1) {  // 取到数据
                if (!audio_pkt) {  // 音频结束标记（nullptr）
                    audio_done = 1;
                } else {
                    // 处理音频Packet：时间基转换 + 写入
                    AVStream *out_stream = ctx->output_format_ctx->streams[1];
                    av_packet_rescale_ts(audio_pkt, 
                                        ctx->audio_encoder_ctx->time_base, 
                                        out_stream->time_base);
                    audio_pkt->pos = -1;

                    if (av_interleaved_write_frame(ctx->output_format_ctx, audio_pkt) < 0) {
                        std::cerr << "写入音频Packet失败" << std::endl;
                    }
                    av_packet_free(&audio_pkt);
                }
            } else if (ret < 0) {  // 队列异常中止
                audio_done = 1;
            }
        } else {
            // 无音频流时，直接标记音频已结束
            audio_done = 1;
        }

        // 3. 检查是否所有流都已结束
        if (video_done && audio_done) {
            break;
        }

        // 4. 短暂休眠，避免CPU空转
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 写入文件尾（关键步骤，确保文件完整）
    av_write_trailer(ctx->output_format_ctx);

    // 释放可能残留的Packet
    if (video_pkt) av_packet_free(&video_pkt);
    if (audio_pkt) av_packet_free(&audio_pkt);
}

// 清理资源
void cleanup(ProcessingContext *ctx) {
    // 停止所有线程
    ctx->quit = true;
    
    // 等待线程结束
    if (ctx->demux_thread.joinable()) ctx->demux_thread.join();
    if (ctx->video_decode_thread.joinable()) ctx->video_decode_thread.join();
    if (ctx->audio_decode_thread.joinable()) ctx->audio_decode_thread.join();
    if (ctx->video_encode_thread.joinable()) ctx->video_encode_thread.join();
    if (ctx->audio_encode_thread.joinable()) ctx->audio_encode_thread.join();
    if (ctx->mux_thread.joinable()) ctx->mux_thread.join();
    
    // 清空队列
    ctx->video_packet_queue.clear();
    ctx->audio_packet_queue.clear();
    ctx->video_frame_queue.clear();
    ctx->audio_frame_queue.clear();
    ctx->video_encoded_packet_queue.clear();
    ctx->audio_encoded_packet_queue.clear();
    
    // 清理OpenGL资源
    glfwTerminate();

    // 关闭编码器
    avcodec_free_context(&ctx->video_encoder_ctx);
    avcodec_free_context(&ctx->audio_encoder_ctx);
    
    // 关闭解码器
    avcodec_free_context(&ctx->video_decoder_ctx);
    avcodec_free_context(&ctx->audio_decoder_ctx);
    
    // 关闭输出文件
    if (ctx->output_format_ctx) {
        if (!(ctx->output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ctx->output_format_ctx->pb);
        }
        avformat_free_context(ctx->output_format_ctx);
    }
    
    // 关闭输入文件
    avformat_close_input(&ctx->input_format_ctx);
    
    // 释放转换器
    sws_freeContext(ctx->sws_ctx);
    swr_free(&ctx->swr_ctx);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "用法: " << argv[0] << " <输入文件> <输出文件> <水印图片>" << std::endl;
        return 1;
    }

    avformat_network_init();

    ProcessingContext ctx;
    ctx.output_filename = argv[2];
    ctx.watermark_path = argv[3];  // 设置水印图片路径

    // 初始化输入
    if (init_input(&ctx, argv[1]) < 0) {
        cleanup(&ctx);
        return 1;
    }

    // 初始化输出
    if (init_output(&ctx) < 0) {
        cleanup(&ctx);
        return 1;
    }

    // 启动线程
    ctx.demux_thread = std::thread(demux_thread_func, &ctx);
    ctx.video_decode_thread = std::thread(video_decode_thread_func, &ctx);
    if (ctx.audio_stream_index != -1) {
        ctx.audio_decode_thread = std::thread(audio_decode_thread_func, &ctx);
    }
    ctx.render_thread = std::thread(render_thread_func, &ctx);
    ctx.video_encode_thread = std::thread(video_encode_thread_func, &ctx);
    if (ctx.audio_stream_index != -1) {
        ctx.audio_encode_thread = std::thread(audio_encode_thread_func, &ctx);
    }
    ctx.mux_thread = std::thread(mux_thread_func, &ctx);

    // 等待所有线程完成
    if (ctx.demux_thread.joinable()) ctx.demux_thread.join();
    if (ctx.video_decode_thread.joinable()) ctx.video_decode_thread.join();
    if (ctx.audio_decode_thread.joinable()) ctx.audio_decode_thread.join();
    if (ctx.render_thread.joinable()) ctx.render_thread.join();
    if (ctx.video_encode_thread.joinable()) ctx.video_encode_thread.join();
    if (ctx.audio_encode_thread.joinable()) ctx.audio_encode_thread.join();
    if (ctx.mux_thread.joinable()) ctx.mux_thread.join();

    // 清理资源
    cleanup(&ctx);

    std::cout << "处理完成，输出文件: " << argv[2] << std::endl;

    return 0;
}