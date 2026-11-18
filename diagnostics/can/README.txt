FluentHMI Studio CAN 诊断版（Windows x64）

用途
默认关闭 CAN 后台周期状态查询，并记录驱动调用进入/返回、耗时、DLL 实际路径、收帧与交付时间。
窗口标题带 [CAN diagnostics]，版本带 -can-diagnostics。
本包使用当前源码的收发和监视逻辑，用于检查修复后的调用耗时及接收交付过程。
具体版本及源码提交见 diagnostic-build.json。

准备
1. 解压到一个单独的可写文件夹。
2. 从你现在能连接创芯 CAN 卡的 HMI 目录，原样复制 zlgcan.dll 和整个 kerneldlls 文件夹到本包的程序旁边。
   如果原来放在 zlgcan 子文件夹中，请原样复制整个 zlgcan 文件夹。
   沿用现有设备驱动和 DLL，不要为本测试更换驱动或混用其他版本的库。
3. 测试时关闭原 HMI 和周立功上位机，释放接收 CAN 卡。

先测关闭状态查询
1. 双击 01-Receive-No-Status.cmd，选择原来的 .uidp 工程。
   也可以将 .uidp 文件拖到该 cmd 文件上。
   如果只有导出应用，请选择该导出目录中的 project.uidp。
2. 使用与待测总线一致的型号、设备、通道、波特率和帧格式连接；两次测试保持相同参数。
3. 保持连接十几秒，再从北京泰科卡只发送一帧，记录你看到的延迟。
4. 等待约10秒，然后断开 CAN 并正常关闭窗口。
5. 日志在本包 logs\status-off 文件夹，每次运行一个新的 .jsonl 文件。

可选的同包对照
双击 02-Receive-With-Status.cmd，选择同一个工程，重复完全相同的单帧测试。
它开启当前版本支持的周期状态查询，并继续记录每个驱动调用，日志在 logs\status-on。
两种模式使用完全相同的 exe、DLL 和报文监视代码。
USBCAN-I/II（SJA1000 系列）在两种模式下都跳过已知可能阻塞的 ZCAN_ReadChannelStatus；
02 对该系列只开启 ZCAN_ReadChannelErrInfo，不恢复旧版的阻塞调用。
如需重现最初的 01 快、02 慢，应使用保留的原始诊断包（源码提交 a64f644），不能用当前包代替。

需要 Studio 预览时
03-Open-Studio.cmd 会启动关闭状态查询的 Studio，日志在 logs\studio-status-off。
直接双击本包 exe 也默认关闭状态查询，日志使用系统默认的可写位置。

回传
把 logs 文件夹打包发回，同时注明：关闭查询时延迟多少、恢复查询时延迟多少。
每行 JSON 包含单调时间；驱动 duration_us 单位为微秒，1000000 微秒 = 1 秒。
关闭状态查询期间，连接标志只表示通道已打开，不用于判断实时总线错误状态。
本包不包含厂商专有 DLL，不会向 GitHub 或其他服务器上传诊断日志。
