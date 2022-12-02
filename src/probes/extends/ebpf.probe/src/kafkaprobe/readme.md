# kafkaprobe简介

本项目应用eBPF与XDP采集分析Kafka的流量，获取topic，IP，port数据。并以topic数据对不同的IP，port分组，统计一段时间的采集数目并输出。

## 使用方法

启动可执行文件kafkaprobe，启动命令如下

```
./kafkaprobe -d eth0 -k 9092 -t 5

```
启动过程中指定的参数有三个。第一个是`-d`，表示kafkaprobe挂载的网卡名。网卡名默认参数是eth0。用户可以通过`ip link show`查看本机当前的网卡名。第二个参数`-k`是指定kafka所监控的port端口，默认值是9092。第三个参数是探针信息输出的时间周期，单位是秒。默认参数是间隔5秒输出。

## 输出参数

输出信息如下所示，
```
|kafkaprobe|Producer|10.244.230.69|49584|1|test_topic|10.243.229.47|9092|
|kafkaprobe|Consumer|10.244.230.69|60254|1|test_topic|10.243.229.47|9092|
```

各字段含义如下：

```
|表名|类型|客户端IP|客户端port|消息数目|topic|服务端IP|服务端port|
```

其中消息数目是两次输出间隔之间捕获的消息数。

## 关于topic数目不准

本方案通过读取kafka server的入流量分析客户端对消息发送与读取的数据。由于只有入流量，难以完整监控一次消息流程。如果存在客户端因为网络问题，导致消息发送失败引发重复获取，可能导致topic数据不准确。

除此之外，在kafka中，客户端对消息的读取不是固定数目，报文中也没有相关信息。现有方案是通过计算两次消息ID的偏移量来计算消息消费的数目。在实验环境中，该方法可以达到99%的准确率。但如果出现用户指定ID获取消息，可能存在数据不准确。
