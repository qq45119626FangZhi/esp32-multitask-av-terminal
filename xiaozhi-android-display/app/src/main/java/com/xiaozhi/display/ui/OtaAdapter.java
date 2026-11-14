package com.xiaozhi.display.ui;

import android.view.LayoutInflater;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.xiaozhi.display.R;
import com.xiaozhi.display.data.OtaPackage;
import com.xiaozhi.display.databinding.ItemOtaPackageBinding;

import java.util.ArrayList;
import java.util.List;

/** OTA 包列表适配器：主标题是文件名，副标题是 "v1.1.0 · 3.8 MB"。 */
public class OtaAdapter extends RecyclerView.Adapter<OtaAdapter.Holder> {

    public interface OnPackageClick {
        void onClick(OtaPackage pkg);
    }

    private final List<OtaPackage> items = new ArrayList<>();
    private final OnPackageClick listener;

    public OtaAdapter(OnPackageClick listener) {
        this.listener = listener;
    }

    public void submit(List<OtaPackage> list) {
        items.clear();
        if (list != null) {
            items.addAll(list);
        }
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        ItemOtaPackageBinding binding = ItemOtaPackageBinding.inflate(
                LayoutInflater.from(parent.getContext()), parent, false);
        return new Holder(binding);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        OtaPackage pkg = items.get(position);
        holder.binding.tvName.setText(pkg.getName());
        holder.binding.tvMeta.setText(pkg.getMetaText());
        int color = holder.binding.getRoot().getContext().getColor(
                pkg.isValid() ? R.color.textSub : R.color.red);
        holder.binding.tvMeta.setTextColor(color);
        holder.binding.getRoot().setOnClickListener(v -> listener.onClick(pkg));
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static class Holder extends RecyclerView.ViewHolder {
        final ItemOtaPackageBinding binding;

        Holder(ItemOtaPackageBinding binding) {
            super(binding.getRoot());
            this.binding = binding;
        }
    }
}
