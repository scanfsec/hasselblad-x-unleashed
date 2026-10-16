/* 3fr.h — 哈苏 .3FR RAW 内嵌 JPEG preview 提取
 *
 * 背景：无真实 3FR 样本，按标准 TIFF 语义 + 哈苏固件线索实现：
 *   - 3FR 是 TIFF 基结构（II/MM 字节序 + magic 42 + IFD 链）；
 *   - 主 IFD 的 StripOffsets/StripByteCounts 指向 PhotoData（原始传感
 *     器数据，非 JPEG）；
 *   - 固件字符串证据（哈苏机身固件）：
 *       E_Resolutions_PreviewFullEmbedded   —— 全尺寸内嵌 preview
 *       MetadataIFDPreview                  —— preview 所在的元数据 IFD
 *     即 preview 以 JPEG 条带（Strip/JPEGInterchangeFormat）挂在 IFD
 *     链/SubIFD 某处。本模块遍历全部 IFD，收集所有「以 SOI(FFD8) 开头」
 *     的条带组，取总字节数最大者（= PreviewFullEmbedded，小的是缩略图），
 *     返回完整 JPEG 字节流，直接喂给 imgprep 管线。
 *   - 真机样本校准：待相机在线（见 NOTES.md §11）。
 */
#ifndef X2P_3FR_H
#define X2P_3FR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    H3FR_OK        = 0,
    H3FR_E_IO      = -1,  /* 文件打开/读取失败 */
    H3FR_E_MEM     = -2,  /* 内存不足 */
    H3FR_E_NOTIFF  = -3,  /* 无效 TIFF 基结构（非 3FR/损坏） */
    H3FR_E_NOPREV  = -4,  /* 遍历完 IFD 未找到可识别的 JPEG preview */
    H3FR_E_TRUNC   = -5,  /* 条带越界/截断（文件损坏） */
};

const char *h3fr_strerror(int err);

/* 提取 path 内最大的内嵌 JPEG preview（PreviewFullEmbedded）。
 * 成功返回 H3FR_OK 并设置 *out_jpeg（malloc 的字节流，以 FFD8 开头，
 * 长度 *out_len，调用方负责 free()）；失败返回上述负值，指针不动。 */
int h3fr_extract_preview(const char *path, unsigned char **out_jpeg,
                         size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* X2P_3FR_H */
