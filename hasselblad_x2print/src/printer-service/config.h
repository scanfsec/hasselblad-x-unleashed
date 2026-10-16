/* config.h — /data/x2print/config.ini 读写(手写最小 ini, 无第三方依赖) */
#ifndef PRN_CONFIG_H
#define PRN_CONFIG_H

/* 载入: 文件不存在则写默认值(首启); 非法键值取默认并告警. 0=ok, -1=io 错 */
int prn_config_load(const char *path);
/* 把可写属性落盘. 0=ok, -1=io 错 */
int prn_config_save(const char *path);

/* ---- 服务级扩展键(阶段2/收官, 非 D-Bus 属性; 见 NOTES.md §10/§16) ----
 * printer_ip      : 已知打印机端点缓存(host[:port] 逗号分隔)。非空时
 *                   Start_discovery 直连快路径优先; 发现学得的新端点回写此键。
 * ipp_port        : IPP 端口(默认 631)
 * photo_root      : Get_photo_list 默认目录(空=相机默认 /mnt/media_runtime)
 * wifi_backend    : Connect_to_ap 执行层 "mock"(联调, 不碰 Wi-Fi)/"cam"(相机编排)
 * idle_restore_min: 闲置看门狗(分钟, 缺省 5, 0=禁用): joined/ready 且无打印任务
 *                   且无业务方法调用超时 → 自动恢复 Wi-Fi 基线回 disconnected
 * 载入时捕获, 保存时原样回写; 未设置返回 "" */
const char *prn_config_get(const char *key);
/* 更新服务级扩展键并原子落盘(load 记住路径); 0=ok, -1=未知键/IO 错 */
int prn_config_set(const char *key, const char *val);

#endif
