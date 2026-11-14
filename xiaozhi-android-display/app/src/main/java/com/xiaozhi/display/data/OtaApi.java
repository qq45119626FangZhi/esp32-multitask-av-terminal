package com.xiaozhi.display.data;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * 下位机 OTA 接口（与 main/internet/xiaozhi_http.c 的 /api/ota/* 对应）：
 *   GET  /api/ota/list                     设备上的升级包 + 当前版本/槽位
 *   POST /api/ota/apply?path=OTA/x.bin     用设备上的包安装（阻塞到写完并重启）
 *   POST /api/ota/upload?name=x.bin&save=1 手机推包（边收边写 flash，同时落卡）
 *   设备成功后会在 1.5 s 后自动重启，所以这两个 POST 都要用长超时。
 * 所有方法都是阻塞调用，必须在子线程执行。
 */
public final class OtaApi {

    private static final int CONNECT_TIMEOUT_MS = 8000;
    /** 普通读取（列表/状态） */
    private static final int READ_TIMEOUT_MS = 6000;
    /** 上传 / 安装：3.9 MB 包在设备侧要写 15~40 s，留足余量 */
    private static final int LONG_READ_TIMEOUT_MS = 240000;

    /** 上传进度回调（在子线程触发） */
    public interface Progress {
        void onProgress(long sent, long total);
    }

    private OtaApi() {
    }

    /** 拉取设备上的包列表与当前版本 */
    public static OtaInfo fetchInfo(String baseUrl) throws IOException {
        String body = httpGet(baseUrl + "/api/ota/list", READ_TIMEOUT_MS);
        try {
            JSONObject o = new JSONObject(body);
            String ver = o.optString("ver", "--");
            String slot = o.optString("slot", "--");
            List<OtaPackage> list = new ArrayList<>();
            JSONArray arr = o.optJSONArray("pkgs");
            if (arr != null) {
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject p = arr.getJSONObject(i);
                    String name = p.optString("name", "");
                    if (name.isEmpty()) {
                        continue;
                    }
                    list.add(new OtaPackage(name, p.optString("path", "OTA/" + name), p.optLong("size", 0L),
                            p.optString("ver", "--"), p.optBoolean("valid", false)));
                }
            }
            return new OtaInfo(ver, slot, list);
        } catch (Exception e) {
            throw new IOException("返回格式不正确（固件可能低于 1.0）");
        }
    }

    /** 用设备上的包安装；成功返回设备 JSON，失败抛 IOException（消息可直接展示） */
    public static String apply(String baseUrl, String relPath) throws IOException {
        String url = baseUrl + "/api/ota/apply?path=" + urlEncode(relPath);
        return httpPost(url, null, 0, null, LONG_READ_TIMEOUT_MS);
    }

    /**
     * 手机推送升级包：请求体就是 app 分区镜像的原始字节。
     * 设备侧会同时保存一份到 /sdcard/OTA（save=1），方便之后在列表里重装/回退。
     */
    public static String upload(String baseUrl, String name, InputStream in, long size, Progress cb)
            throws IOException {
        String url = baseUrl + "/api/ota/upload?name=" + urlEncode(name) + "&save=1";
        return httpPost(url, in, size, cb, LONG_READ_TIMEOUT_MS);
    }

    /** 手动重启设备（当前设计升级成功后会自动重启，保留以便排障） */
    public static void reboot(String baseUrl) throws IOException {
        httpPost(baseUrl + "/api/ota/reboot", null, 0, null, READ_TIMEOUT_MS);
    }

    // ------------------------------------------------------------------

    private static String httpGet(String urlText, int readTimeout) throws IOException {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(urlText).openConnection();
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(readTimeout);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("Connection", "close");
            int code = conn.getResponseCode();
            String body = readAll(code >= 400 ? conn.getErrorStream() : conn.getInputStream());
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException(parseError(body, code));
            }
            return body;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static String httpPost(String urlText, InputStream in, long size, Progress cb, int readTimeout)
            throws IOException {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(urlText).openConnection();
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(readTimeout);
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setRequestProperty("Connection", "close");

            if (in != null && size > 0) {
                conn.setRequestProperty("Content-Type", "application/octet-stream");
                // 3.9 MB 不能整块进内存：用定长流式上传
                conn.setFixedLengthStreamingMode(size);
                try (OutputStream out = conn.getOutputStream()) {
                    byte[] buf = new byte[16384];
                    long sent = 0;
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        sent += n;
                        if (cb != null) {
                            cb.onProgress(sent, size);
                        }
                    }
                    out.flush();
                }
            } else {
                conn.setFixedLengthStreamingMode(0);
                // 无请求体也要触发一次连接，让服务端进入处理流程
                try (OutputStream out = conn.getOutputStream()) {
                    out.flush();
                }
            }

            int code = conn.getResponseCode();
            String body = readAll(code >= 400 ? conn.getErrorStream() : conn.getInputStream());
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException(parseError(body, code));
            }
            return body;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    /** 设备返回 {"ok":false,"err":"..."} 时优先展示 err */
    private static String parseError(String body, int code) {
        if (body != null && !body.isEmpty()) {
            try {
                JSONObject o = new JSONObject(body);
                String err = o.optString("err", "");
                if (!err.isEmpty()) {
                    return err;
                }
            } catch (Exception ignored) {
                // 非 JSON，用原文
            }
            if (body.length() <= 120) {
                return body.trim();
            }
        }
        return "HTTP " + code;
    }

    private static String readAll(InputStream in) throws IOException {
        if (in == null) {
            return "";
        }
        try (InputStream is = in) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream(512);
            byte[] buf = new byte[512];
            int n;
            while ((n = is.read(buf)) > 0) {
                bos.write(buf, 0, n);
            }
            return new String(bos.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static String urlEncode(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8");
        } catch (Exception e) {
            return s;
        }
    }
}
