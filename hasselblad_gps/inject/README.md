# hginject — Hasselblad 3FR / JPG / HEIC 照片 GPS 注入器(单文件 C99)

`hginject.c` 是 `x2gps/geoinject.c`(2026-10-08 真机验证)的成品化移植:
3FR 沿用已验证的"文件尾追加 + 0x8769 重指"方案,JPG 保留 XMP 通路并新增
EXIF APP1 追加通路,HEIC 按 `docs/X2DII_PORT_ANALYSIS.md` §3 的盒树规格在
Exif item 32KB extent 尾隙原位注入。GPS IFD 标签集升级为
`docs/GPS_LIFECYCLE_X1D_X1DII.md` §3.4 的生命周期规格。三种格式全部
**追加 + 单点重指**:相机写入的任何字节区间都不搬移,revert 逐字节精确
(真机样本 md5 往返一致,见 `test/TESTLOG.md`)。

```
hginject <file> --lat F --lon F [--alt F] [--dop F] [--speed F(km/h)]
                   [--track F] [--nsat N] [--fix-valid 0|1]
                   [--utc "YYYY-MM-DDTHH:MM:SSZ"] [--datum WGS-84] [--force]
hginject --revert <file>            # 从文件内容识别自身注入并精确撤销

exit: 0 ok · 1 skipped(已注入/无可撤销) · 2 usage · 3 I/O
      4 格式/结构错误(含不满足精确撤销前提) · 5 空间不足(HEIC 尾隙/APP1 上限)
```

构建(host 与 aarch64 Android 双端,零第三方依赖,零警告):

```
gcc -std=c99 -Wall -Wextra -O2 -o hginject hginject.c
clang --target=aarch64-linux-android21 -std=c99 -Wall -Wextra -O2 -o hginject hginject.c
```

(本工具不依赖 libdbus;相机端与 dbus 服务的集成属另一模块。)

JPG 使用 XMP 时，显式传入的 `--utc` 写入 `exif:GPSTimeStamp`，海拔以毫米精度的有理数和海拔参考方向存储。调用方应传入与坐标同一次定位的 UTC。

---

## 1. 格式识别

按魔数优先、扩展名无关:`FFD8`→JPG;`bytes[4:8]=="ftyp"`→HEIC;
`II*\0 / MM\0*`→3FR(TIFF)。TIFF 双端序均支持(样本均为 II)。

## 2. GPS IFD 标签集(生命周期规格)

必写(11 项):`0x0000 GPSVersionID=2.3.0.0`、`0x0001/02` 纬度+Ref、
`0x0003/04` 经度+Ref、`0x0007 GPSTimeStamp`(UTC hh/mm/ss,3×RATIONAL/1)、
`0x0009 GPSStatus`("A"/"V" = --fix-valid)、`0x000A GPSMeasureMode="2"`、
`0x0012 GPSMapDatum`(--datum,默认 "WGS-84")、`0x001D GPSDateStamp`
("YYYY:MM:DD")、`0x001E GPSDifferential=0`。
有值才写:`0x0005/06` 海拔(+Ref 0/1)、`0x0008` 卫星数(ASCII 十进制,
≤2 位时按 EXIF 规则内联)、`0x000B` DOP、`0x000C/0D` 速度(Ref="K",km/h)、
`0x000E/0F` 方位(Ref="T")。
不写:0x10/11、0x13-1C(两代机身均无数据来源,词汇表按可用性裁剪——
见生命周期文档 §3.4 硬边界)。

rational 编码与 geoinject 逐字节同源:`frac_rat()` 以 1e6 分母精度 + gcd
约分;坐标 DMS 换算公式一致(`(int)` 度、分、秒链),读回误差 <1e-6 度。
**值偏移一律写成 TIFF 头相对绝对偏移**(EXIF 标准语义),因此整个追加
结构可被任意标准解析器(PIL/exiftool/libheif)沿指针链读通——这一点与
geoinject 原 IFD 相对写法不同,是本移植的规格修正。

## 3. 3FR(TIFF)内部结构与偏移修正清单

原文件头部(样本 B0000917.3FR):TIFF II,IFD0@+0x8(31 项)→
`0x8769`→ExifIFD@+0x4b6(22 项)、`0x014a`→SubIFD@+0x3b4(17 项,无 0x8769);
主 JLS strip@0xc8cb000、缩略图 strip@0x8000 均远离头部。

注入(geoinject 已验证方案,`写入 <1KB,不动 JLS 数据`):

```
[原文件字节不动]
+pad(0-3B,EOF 4 对齐)
[新 Exif IFD 副本:原 22 项逐字节拷贝 + 0x8825(LONG,1)=gps_off 按升序插入,next=0]
[GPS IFD]
[HGT1 trailer 12B]
```

偏移修正(仅两处,均 4 字节 pwrite):
1. IFD0 内 `0x8769` 值:原 Exif 偏移(0x4b6)→ 文件尾新副本(0xcb72000);
2. SubIFD 内若有 `0x8769`(本样本无,B0000927 有)→ 同值。
Exif 副本各项的值偏移仍指向原头部数据区(未搬移,天然有效);
0x0111/0x117 等 strip 指针不触碰。

revert:读尾部 12B trailer(`HGT1` + 原始文件长度 + 原 Exif 偏移)→
校验"尾部 Exif 副本 − 0x8825 == 头部原 Exif IFD"逐项相等 → 恢复 0x8769、
`ftruncate` 回原始长度(含 pad 字节,精确)。trailer 缺失时回落到
geoinject 的结构扫描法(按"表项一致"在头部搜原 IFD,截断到尾块起点)。

## 4. JPG 内部结构(EXIF 优先,XMP 兜底)

段扫描:SOI 起 walker 到 SOS,APP1 判 `Exif\0\0` / `http://ns.adobe.com/xap/1.0/`。

**路径 1|EXIF APP1 存在** → TIFF 末尾追加 + 段扩容:
```
[FFD8][APP1: FFE1 len][Exif\0\0 + TIFF(不动)]  ←  段长 +Δ
                                        ↑ TIFF 末尾追加:
   [IFD0 副本(原 n 项 + 0x8825=gps_off, next 拷贝)][GPS IFD][trailer]
   TIFF 头 IFD0 指针(4B)→ 指向副本
```
扩容 = ftruncate 增长 → 1MB 块反向搬移段后文件尾 → 写入追加块 → 改 4B
指针 + 2B 段长 → fsync。JPEG 无绝对文件偏移、IFD 偏移 TIFF 相对,故安全。
**不采用"IFD0 原地插项 +12 搬移"**:那要修 ExifIFD/MakerNote 内全部指针,
而哈苏 MakerNote 内部是否含 TIFF 绝对指针未证实——副本+重指方案只动 6 字节。
revert:trailer 记录原 IFD0 偏移与原段长 → 几何互验(0x8825 值 == 副本起点
+表长;追加块尾 == TIFF 末)→ 恢复指针/段长 → 左移 Δ → ftruncate。精确。

**路径 2|仅 XMP APP1 存在(geoinject 方案)** → 在包尾 padding 内
`</rdf:RDF>` 前插 `exif:GPS* rdf:Description`,段长不变(尾 padding 吸收)。
前提:padding 均一字符(注入时校验,否则 exit 4)→ revert 把尾部 tl 字节
按该字符回填,逐字节精确。

**路径 3|两者皆无**(B0000917_heic_preview.jpg 即此类) → 在 SOI/APP0 后
新建最小 XMP APP1(xpacket 包 + 1024B padding + 尾部 trailer),
revert = 整段删除(左移+ftruncate),精确。

## 5. HEIC 内部结构(按 X2DII_PORT_ANALYSIS §3 盒树规格)

定位:顶层盒到 `meta`(先于 mdat)→ `iinf`(infe v2, item_type=='Exif')
得 item id → `iloc`(v1,v0/v2 亦支持;offset/length size 必须 4/4)得
method=0 的唯一 extent。样本:extent=[0xC00, 0x8C00) 32KB,内容 =
`u32BE(6) + "Exif\0\0" + TIFF(II)`,IFD0@TIFF+0x8(15 项)→ExifIFD@+0x12E
(22 项,MakerNote 29226B 到 +0x74F8)→ 尾隙 [0x74F8,0x7FF6) ≈2.8KB 全零。

注入(原位,不动任何盒长/mdat/iloc):
1. 走 IFD 链算 TIFF 内容高水位 hw=0x74F8;
2. 前提校验:[hw, extent 末) 全零(保证 revert 精确)且
   `hw + need ≤ extent TIFF 空间`(不足→ **exit 5**,绝不改盒);
3. 尾隙追加 `[对齐 0-1B][IFD0 副本+0x8825][GPS IFD][trailer]`;
4. TIFF 头 IFD0 指针(4B)→ 副本(0x74F8)。
   **与文档 §3.4 方案 A 的 +12 memmove 实现不同、目标相同**(0x8825 进在位
   IFD0、GPS 落 TIFF 尾、盒结构零变更):选择副本+重指是因为它不搬移
   MakerNote(其内部指针未证实安全),且与 3FR 已验证方案同构——单一
   4 字节指针修补。revert = 指针还原 + 尾隙回填 0。

## 6. 通用机制

- **trailer**(12B,LE):`'H','G','T','1'` + u32 a + u32 b。3FR:a=原始
  文件长、b=原 Exif 偏移;JPG-EXIF:a=原 IFD0 偏移、b=原段长;JPG-XMP 新建:
  a=3;HEIC:a=原 IFD0 偏移、b=0。GPS IFD next=0 终止链,标准解析器永不
  读到它;revert 靠它"从文件内容识别自身注入"并做到含 pad 字节的精确还原。
- **指纹校验**:revert 前用 `gps_is_ours()` 验 GPS IFD(标签严格升序、
  0x0000=2.3.0.0、Status∈{A,V}、MeasureMode="2"、MapDatum 前缀 WGS-84、
  Differential=0、值偏移绝对且落在块内)——**别家工具/原厂写的 GPS 一律
  拒撤(exit 1)**,--force 也不破坏。
- **幂等**:3FR/HEIC/JPG-EXIF 见 0x8825、JPG-XMP 见 `exif:GPSLatitude`
  → 报告已注入并跳过(exit 1);`--force` = 先 revert 再注入。
- **写策略**:原位 pread/pwrite + fsync(geoinject 惯例)。理由:213MB 3FR
  在相机 exFAT SSD 上不可接受临时文件全量拷贝(且 /tmp 仅 32MB tmpfs);
  追加+指针修补的写集 <1KB,半途崩溃留下的尾块可被 trailer/指纹识别并
  干净撤销。JPG 段扩容的文件尾搬移用 1MB 块循环,相机 JPG(≤100MB 量级)
  耗时可忽略。
- 无 memmem/strcasecmp 等 GNU 扩展,自带 `find_blob`;`_POSIX_C_SOURCE
  200809L` 下 glibc 与 bionic 均零警告编译。

## 7. 验证(摘要,全量见 test/TESTLOG.md)

34/34 检查通过:三格式真机样本注入→verify_gps.py 读回(<1e-6)→revert→
**md5 与原件逐字节一致**;幂等 skip(1)/--force(0)/错误码 1·2·4·5;
host 213MB 3FR 注入 0.184 s(原生 ext4,含 fsync)、revert 0.006 s;
NDK aarch64-linux-android21 编译+完整链接零警告(`hginject_aarch64.o`、
`hginject.aarch64`)。
