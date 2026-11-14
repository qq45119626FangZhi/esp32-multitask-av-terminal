package com.xiaozhi.display.data;

import java.util.Locale;

/**
 * 下位机 SD 卡 /sdcard/OTA 目录里的一个升级包。
 * version 是下位机从镜像头（esp_app_desc）里解析出的固件版本。
 */
public class OtaPackage {

    private final String name;
    private final String path;   // 相对 /sdcard，例如 OTA/xxx.bin
    private final long size;
    private final String version;
    private final boolean valid; // 下位机校验镜像头是否通过

    public OtaPackage(String name, String path, long size, String version, boolean valid) {
        this.name = name;
        this.path = path;
        this.size = size;
        this.version = version;
        this.valid = valid;
    }

    public String getName() {
        return name;
    }

    public String getPath() {
        return path;
    }

    public long getSize() {
        return size;
    }

    public String getVersion() {
        return version;
    }

    public boolean isValid() {
        return valid;
    }

    /** 人类可读大小：3.8 MB */
    public String getSizeText() {
        double v = size;
        String[] unit = {"B", "KB", "MB", "GB"};
        int i = 0;
        while (v >= 1024.0 && i < unit.length - 1) {
            v /= 1024.0;
            i++;
        }
        if (i == 0) {
            return size + " B";
        }
        return String.format(Locale.US, "%.1f %s", v, unit[i]);
    }

    /** 列表右侧的说明：v1.1.0 · 3.8 MB（非法包额外标注） */
    public String getMetaText() {
        StringBuilder sb = new StringBuilder();
        if (version != null && !version.isEmpty() && !"--".equals(version)) {
            sb.append('v').append(version).append(" · ");
        }
        sb.append(getSizeText());
        if (!valid) {
            sb.append(" · 镜像无效");
        }
        return sb.toString();
    }
}
