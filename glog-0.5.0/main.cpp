#include <sstream>
#include <iostream>
#include <map>
#include <unordered_map>
#include "test.h"
#define GLOG_NO_ABBREVIATED_SEVERITIES
#include "glog/logging.h"

int main(int argc, char* argv[])
{
    std::unordered_multimap<std::string, int> m1;
    std::map<int, std::unordered_multimap<std::string, int>> m2;

    std::unordered_multimap<std::string, int> m3;
    m3.insert({ "1", 1 });
    m3.insert({ "2", 2 });

    m1.insert(m3.begin(), m3.end());
    m2[1].insert(m3.begin(), m3.end());

    Sleep(5000);

    //google::SetLogDestination(google::GLOG_INFO, "log/prefix_");  //设置特定严重级别的日志的输出目录和前缀。第一个参数为日志级别，第二个参数表示输出目录及日志文件名前缀
    //google::SetLogFilenameExtension("logExtension");  //在日志文件名中级别后添加一个扩展名。适用于所有严重级别
    //google::SetStderrLogging(google::GLOG_INFO);  //大于指定级别的日志都输出到标准输出

    FLAGS_log_dir = "D:\\Logs";
    //FLAGS_logtostderr = true;  //设置日志消息是否转到标准输出而不是日志文件
    //FLAGS_alsologtostderr = true;  //设置日志消息除了日志文件之外是否输出到标准输出
    FLAGS_colorlogtostderr = true;  //设置记录到标准输出的颜色消息（如果终端支持）
    //FLAGS_log_prefix = true;  //设置日志前缀是否应该添加到每行输出
    //FLAGS_logbufsecs = 0;  //设置可以缓冲日志的最大秒数，0指实时输出
    FLAGS_max_log_size = 10;  //设置最大日志文件大小（以MB为单位）,超过会对文件进行分割
    FLAGS_stop_logging_if_full_disk = true;  //设置是否在磁盘已满时避免日志记录到磁盘
    //FLAGS_minloglevel = google::GLOG_WARNING;  //设置最小处理日志的级别
    google::InitGoogleLogging(argv[0]);

    std::string s1(100000, 'c');
    for (int i = 0; i < 1000; ++i) {
        std::string s2 = std::to_string(i);
        LOG(INFO) << s1;
        LOG(INFO) << s2;
    }
    // ...
    LOG(INFO) << "Found " << 1 << " cookies";

    std::stringstream ss;
    ss.str("23232323");
    std::cout << ss.str() << std::endl;
    ss << "fawefxcvasf";
    std::cout << ss.str() << std::endl;
    ss << "sfavxasf";
    std::cout << ss.str() << std::endl;

    Sleep(5000);
    google::ShutdownGoogleLogging();
}