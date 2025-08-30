# Ffmpeg-transform
使用C++ffmpeg和C++opengl来实现对输入视频的格式转换和添加水印以及倍速播放、着色器特效等。

第一次commit完成了整体的流程，输出了转码后的视频文件（带音频）

第二次commit完成了渲染管线部分，输出文件为空，是由于opengl部分在decode线程内不是线程安全的。

第三次commit将渲染管线部分单独作为一个线程。
