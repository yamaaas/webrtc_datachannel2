extern "C" {
#include <time.h>
#include <termios.h>
#include <fcntl.h>    /* For O_RDWR */
#include <unistd.h>   // read, close
#include <stdlib.h>   // sscanf
#include <string.h>   // memset, strstr // *** strstr を使うために追加 ***
}

#include <string>
#include <cstring> // std::strlen などC文字列操作が必要な場合
#include <cmath>
#include <regex> 
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <iostream> 
#include "std_msgs/msg/float32.hpp"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp" 
#include "std_msgs/msg/bool.hpp"   // *** 追加 *** : 動力制御コマンド用
#include "geometry_msgs/msg/twist.hpp"

#define BUFSIZE 128
#define WRITE_BUFSIZE 64

class DataChannel : public rclcpp::Node {
public:
    DataChannel(): Node("webrtc_datachannel"), port(-1), is_open(false) // portを初期化
    {
        this->declare_parameter("serial_port", std::string("/dev/pts/2"));
        // *** 追加: 動力制御コマンド用トピック名のパラメータ ***
        this->declare_parameter("power_control_topic", std::string("/power_control/command"));
        this -> declare_parameter("lidar_command_topic", std::string("iswork"));
        this -> declare_parameter("vbat_topic", std::string("/vbat"));

        serial_port_name_ = this->get_parameter("serial_port").as_string();
        std::string power_topic = this->get_parameter("power_control_topic").as_string();
        std::string vbat_topic_name = this -> get_parameter("vbat_topic").as_string();
        std::string lidar_topic = this -> get_parameter("lidar_command_topic").as_string();

        cmd_vel_pub_ = this->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
        // *** 追加: 動力制御コマンド用パブリッシャーの初期化 ***
        power_command_pub_ = this->create_publisher<std_msgs::msg::Bool>(
            power_topic, rclcpp::QoS(rclcpp::KeepLast(1)).reliable()); // QoS設定
        rclcpp::QoS vbat_qos(rclcpp::KeepLast(1));    
        vbat_sub_ = this -> create_subscription<std_msgs::msg::Float32>(
            vbat_topic_name,
            vbat_qos,
            std::bind(&DataChannel::vbatCallback, this, std::placeholders::_1)
            );

        //lidar avoidance ONOFF
        lidar_command_pub_ = this->create_publisher<std_msgs::msg::Bool>(
            lidar_topic, rclcpp::QoS(rclcpp::KeepLast(1)).reliable()); // QoS設定
        
        loop_timer_ = this->create_wall_timer(std::chrono::milliseconds(50), std::bind(&DataChannel::loop, this));

        is_open = false;
        if (!openPort(serial_port_name_)) { // openPortの引数をconst参照に変更
             RCLCPP_ERROR(this->get_logger(), "Failed to open serial port %s initially.", serial_port_name_.c_str());
        } else {
             last_rcv = time(NULL); // 正常に開けたら初期化
        }
    }


private:
    static const int BAUDRATE = B115200;
    bool openPort(const std::string& serial_port);
    bool loop();
    // *** 変更: 引数を const char* に (既存の呼び出し方に合わせる) ***
    void pubMsg(const char* msg_buf); // C文字列を受け取るように変更

    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub_;
    // *** 追加: 動力制御コマンド用パブリッシャー ***
    rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr power_command_pub_;
    rclcpp::Subscription<std_msgs::msg::Float32>::SharedPtr vbat_sub_;
    rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr lidar_command_pub_;
    rclcpp::TimerBase::SharedPtr loop_timer_;
    std::string serial_port_name_;
    int port;
    bool is_open;
    char buf[BUFSIZE]; // シリアル読み取りバッファ

    time_t last_rcv;

  void vbatCallback(const std_msgs::msg::Float32::ConstSharedPtr msg){
    RCLCPP_INFO(this -> get_logger(), "vbatCallback triggered with value : %.3f", msg -> data);
    if(!is_open || port < 0){
      RCLCPP_WARN_THROTTLE(this -> get_logger(), *this -> get_clock(), 5000, "Serial port no open, cannnot write vbat data");
      return;
    }

    char write_buf[WRITE_BUFSIZE];

    int len = snprintf(write_buf, WRITE_BUFSIZE, "vbat:%.3f\n", msg -> data);

    if(len < 0 || len >= WRITE_BUFSIZE){
      RCLCPP_ERROR(this -> get_logger(), "Failed to format vbat message or buffer too small");
      return;
    }

    ssize_t byte_written = write(port, write_buf,len);

    if(byte_written < 0){
      if(errno != EAGAIN && errno != EWOULDBLOCK){
        RCLCPP_ERROR(this -> get_logger(), "Serial write error: %s (errno %d)", strerror(errno), errno);
      }
      else{
        RCLCPP_DEBUG(this -> get_logger(), "Serial write would block (EAGAIN/EWOULDBLOCK");
      }
    }
    else if(byte_written < len){
      RCLCPP_WARN(this -> get_logger(), "Partial serial write occured (%zd / %d bytes).", byte_written, len);
    }
    else{
      constexpr int precision = std::numeric_limits<double>::max_digits10;
      RCLCPP_DEBUG(this -> get_logger(), "Wrote vbat data to serial: %.*f",precision, msg -> data);
    }
  }
};




bool DataChannel::loop() {
    memset(buf, '\0', BUFSIZE);
    // readはノンブロッキングなので、データがなければ0、エラーなら-1を返す
    int ret = read(port, buf, BUFSIZE - 1); // null終端を確保
    time_t tnow = time(NULL);

    if (ret > 0) { // データを受信した場合
        //last_rcv = tnow; // 最後に受信した時刻を更新
        last_rcv = time(NULL);
        pubMsg(this->buf); // 受信したバッファをそのまま渡す
    } else if (ret < 0) { // エラー発生
        // EAGAIN or EWOULDBLOCK はノンブロッキングでは正常なので無視
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            RCLCPP_ERROR(this->get_logger(), "Serial read error: %s (errno %d)", strerror(errno), errno);
            RCLCPP_INFO(this->get_logger(), "Serial read error occurred (non-blocking): %s (errno %d)", strerror(errno), errno);
            // エラー時はポートを閉じる (再接続は次のループで試行)
            //close(port);
            //is_open = false;
            //port = -1;
            fprintf(stderr, "%s:%s:%d: read error\n",
                            __FILE__, __func__, __LINE__ );
        }
    }
    // 一定時間受信がなければ速度を0にする (既存の処理)
    if ((tnow - last_rcv) >= 2) {
        pubMsg("{\"x\":0,\"z\":0}"); // 停止コマンドを送信
    }
    return true;
}

// *** 引数を const char* に変更 ***
void DataChannel::pubMsg(const char* msg_buf) {
    // *** 追加: まず power_on コマンドかチェック ***
    // strstr は C の関数で、文字列内に部分文字列が含まれるか検索する
    if (strstr(msg_buf, "power_on:") != NULL) {
        RCLCPP_INFO(this->get_logger(), "Found 'power_on:' in buffer.");
        auto power_msg = std_msgs::msg::Bool();
        bool command_recognized = false;
        // "true" または "false" を探す (引用符込み)
        if (strstr(msg_buf, "\"true\"") != NULL) {
          RCLCPP_INFO(this->get_logger(), "Found '\"true\"' in buffer.");
            power_msg.data = true;
            command_recognized = true;
            RCLCPP_INFO(this->get_logger(), "Received Power ON command via serial.");
        } else if (strstr(msg_buf, "\"false\"") != NULL) {
          RCLCPP_INFO(this->get_logger(), "Found '\"false\"' in buffer.");
            power_msg.data = false;
            command_recognized = true;
            RCLCPP_INFO(this->get_logger(), "Received Power OFF command via serial.");
        } else {
          RCLCPP_WARN(this->get_logger(), "Found 'power_on:' but unrecognized value: [%s]", msg_buf);
            //RCLCPP_WARN(this->get_logger(), "Unrecognized value in power_on command: %s", msg_buf);
        }

        if (command_recognized) {
            power_command_pub_->publish(power_msg);
            RCLCPP_INFO(this->get_logger(), ">>> Publishing power command: %s", power_msg.data ? "true" : "false");
        }
        return; // 動力コマンドを処理したのでここで終了
    }
    else if(strstr(msg_buf, "lidar_on:")!= NULL){
        RCLCPP_INFO(this -> get_logger(), "Found 'lidar_on:' in buffer.");
        auto lidar_msg = std_msgs::msg::Bool();
        bool command_recognized = false;   
        if (strstr(msg_buf, "\"true\"") != NULL) {
          RCLCPP_INFO(this->get_logger(), "Found '\"true\"' in buffer.");
            lidar_msg.data = true;
            command_recognized = true;
            RCLCPP_INFO(this->get_logger(), "Received Power ON command via serial.");
        } else if (strstr(msg_buf, "\"false\"") != NULL) {
          RCLCPP_INFO(this->get_logger(), "Found '\"false\"' in buffer.");
            lidar_msg.data = false;
            command_recognized = true;
            RCLCPP_INFO(this->get_logger(), "Received Power OFF command via serial.");
        } else {
          RCLCPP_WARN(this->get_logger(), "Found 'lidar_on:' but unrecognized value: [%s]", msg_buf);
            //RCLCPP_WARN(this->get_logger(), "Unrecognized value in power_on command: %s", msg_buf);
        }
        if(command_recognized){
            lidar_command_pub_ -> publish(lidar_msg);
            RCLCPP_INFO(this->get_logger(), ">>> Publishing lidar command: %s", lidar_msg.data ? "true" : "false");
        }
        return;
    }
    RCLCPP_INFO(this->get_logger(), "Buffer does not contain 'power_on:' and 'lidar_on', attempting Twist parse for: [%s]", msg_buf);
    // *** 以下は既存の速度コマンド処理ロジック (power_on: でなければ実行) ***
    double x = 0.0, z = 0.0;
    auto pubmsg = geometry_msgs::msg::Twist();

    // 既存のコードでは改行文字の除去を regex_replace で行っていたが、
    // loop 内で受信バッファをそのまま渡す形になったため、
    // ここでの除去は不要かもしれない (Momoからのデータ形式による)
    // 必要であれば、ここで除去処理を追加する。
    std::string nmsg = std::regex_replace(msg_buf, std::regex("[\n\r]"), "");
    printf("%s\n", nmsg.data()); // 既存のデバッグ出力

    // sscanf でパース (既存のコード)
    // 注意: msg_buf がJSON形式でない場合、sscanf は失敗する可能性がある
    int ret = std::sscanf(msg_buf, "%*[^{]{\"x\":%lf,\"z\":%lf}", &x, &z); // 既存のパース処理に近くする
    
   if(ret < 0){
      fprintf(stderr, "%s:%s:%d: sscanf error\n", __FILE__, __func__, __LINE__);
    }
    else{
      printf("x: %f, z: %f\n", x, z);
    }
    pubmsg.linear.x = x;
    pubmsg.angular.z = z;
    cmd_vel_pub_ -> publish(pubmsg);
}


bool DataChannel::openPort(const std::string& serial_port) {
  
  if((port = open(serial_port.c_str(), O_RDWR | O_NONBLOCK)) < 0){
    fprintf(stderr, "%s: can't open port(%s)\n", __func__, serial_port.c_str());
    return false;
  }

  struct termios p;
  tcgetattr(port, &p);

  memset(&p, 0, sizeof(struct termios));
  p.c_iflag = IGNPAR;
  p.c_cflag = BAUDRATE | CS8 | CREAD | CLOCAL;

  tcsetattr(port, TCSAFLUSH, const_cast < const termios*>(&p));

  is_open = true;

  return true;
}


int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    // *** DataChannel のインスタンス作成と実行 ***
    auto data_channel_node = std::make_shared<DataChannel>();
    rclcpp::spin(data_channel_node);
    rclcpp::shutdown();

    return 0;
}
