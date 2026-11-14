package com.xiaozhi.display.ui;

import android.content.Intent;
import android.os.Bundle;
import android.text.InputType;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.lifecycle.ViewModelProvider;

import com.xiaozhi.display.R;
import com.xiaozhi.display.data.DeviceConfig;
import com.xiaozhi.display.databinding.ActivityMainBinding;
import com.xiaozhi.display.viewmodel.Event;
import com.xiaozhi.display.viewmodel.MainViewModel;

/**
 * 首页：标题显示下位机当前模式，中间 60% 区域显示监控画面，底部两个按钮。
 * 拉流与网络请求全部在 ViewModel 的子线程里完成。
 */
public class MainActivity extends AppCompatActivity {

    /** 监控画面区域占屏幕高度的比例 */
    private static final float STREAM_HEIGHT_RATIO = 0.60f;

    private ActivityMainBinding binding;
    private MainViewModel viewModel;

    private boolean online;
    private boolean streaming;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        viewModel = new ViewModelProvider(this).get(MainViewModel.class);

        applyStreamHeight();
        setupViews();
        observeViewModel();
    }

    /** 中间画面严格占屏幕高度的 60%，其余空间留给标题与按钮 */
    private void applyStreamHeight() {
        int screenHeight = getResources().getDisplayMetrics().heightPixels;
        ViewGroup.LayoutParams lp = binding.streamBox.getLayoutParams();
        lp.height = (int) (screenHeight * STREAM_HEIGHT_RATIO);
        binding.streamBox.setLayoutParams(lp);
    }

    private void setupViews() {
        binding.btnSwitchMode.setOnClickListener(v -> viewModel.switchNextMode());
        binding.btnSdFiles.setOnClickListener(v -> startActivity(new Intent(this, FileBrowserActivity.class)));
        // 最下一行：OTA 固件升级（设备包列表安装 + 手机选包推送）
        binding.btnOta.setOnClickListener(v -> startActivity(new Intent(this, OtaActivity.class)));

        // 长按标题：修改板子地址（板子 IP 由 DHCP 分配，换网络时需要改）
        binding.tvTitle.setOnLongClickListener(v -> {
            showBaseUrlDialog();
            return true;
        });

        updateConnText();
    }

    private void observeViewModel() {
        viewModel.getMode().observe(this, mode -> binding.tvTitle.setText(
                getString(R.string.title_prefix) + modeLabel(mode)));

        viewModel.getOnline().observe(this, isOnline -> {
            online = Boolean.TRUE.equals(isOnline);
            updateConnText();
        });

        viewModel.getStreaming().observe(this, isStreaming -> {
            streaming = Boolean.TRUE.equals(isStreaming);
            if (!streaming) {
                // 无推流数据 -> 画面区域保持空白
                binding.ivStream.setImageDrawable(null);
            }
            updateConnText();
        });

        viewModel.getFrame().observe(this, bitmap -> {
            if (bitmap != null) {
                binding.ivStream.setImageBitmap(bitmap);
            }
        });

        viewModel.getToast().observe(this, event -> {
            String text = consume(event);
            if (text != null) {
                Toast.makeText(this, text, Toast.LENGTH_SHORT).show();
            }
        });

        // 右上角：连接状态 + 已解码帧数（现场定位拉流问题用）
        viewModel.getStreamInfo().observe(this, text -> binding.tvConnState.setText(text));
    }

    private String consume(Event<String> event) {
        return event == null ? null : event.getContentIfNotHandled();
    }

    private void updateConnText() {
        String text = viewModel.getStreamInfo().getValue();
        binding.tvConnState.setText(text == null ? "未连接" : text);
    }

    private String modeLabel(String mode) {
        if (mode == null) {
            return getString(R.string.mode_unknown);
        }
        switch (mode) {
            case "voice":
                return getString(R.string.mode_voice);
            case "doorbell":
                return getString(R.string.mode_doorbell);
            case "monitor":
                return getString(R.string.mode_monitor);
            default:
                return mode;
        }
    }

    private void showBaseUrlDialog() {
        EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        input.setText(DeviceConfig.getBaseUrl(this));
        input.setSelection(input.getText().length());

        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        FrameLayout container = new FrameLayout(this);
        container.setPadding(pad, pad, pad, 0);
        container.addView(input, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(this)
                .setTitle("板子地址")
                .setView(container)
                .setPositiveButton("保存", (dialog, which) -> {
                    String url = DeviceConfig.normalize(input.getText().toString());
                    DeviceConfig.setBaseUrl(this, url);
                    viewModel.rebase(url);
                    Toast.makeText(this, "已切换：" + url, Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    @Override
    protected void onStart() {
        super.onStart();
        viewModel.start(DeviceConfig.getBaseUrl(this));
    }

    @Override
    protected void onStop() {
        viewModel.stop();
        super.onStop();
    }
}
