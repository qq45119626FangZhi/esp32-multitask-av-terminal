package com.xiaozhi.display.data;

import java.util.Collections;
import java.util.List;

/**
 * 下位机 OTA 页一次拉取的结果：当前固件版本 + 启动槽 + 设备上的升级包列表。
 */
public class OtaInfo {

    private final String version;
    private final String slot;
    private final List<OtaPackage> packages;

    public OtaInfo(String version, String slot, List<OtaPackage> packages) {
        this.version = version == null ? "--" : version;
        this.slot = slot == null ? "--" : slot;
        this.packages = packages == null ? Collections.emptyList() : packages;
    }

    public String getVersion() {
        return version;
    }

    public String getSlot() {
        return slot;
    }

    public List<OtaPackage> getPackages() {
        return packages;
    }

    /** 界面显示："V1.0"（只取版本前两段） */
    public String getShortVersion() {
        String[] parts = version.split("\\.");
        if (parts.length >= 2) {
            return "V" + parts[0] + "." + parts[1];
        }
        return version;
    }
}
