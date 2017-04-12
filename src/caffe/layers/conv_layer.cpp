#include <vector>

#include "caffe/layers/conv_layer.hpp"

namespace caffe {

// Feature Map 输出维度
template <typename Dtype>
void ConvolutionLayer<Dtype>::compute_output_shape() {
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int* stride_data = this->stride_.cpu_data();
  const int* pad_data = this->pad_.cpu_data();
  const int* dilation_data = this->dilation_.cpu_data();
  this->output_shape_.clear();
  // 计算每一个 spatial axis 的输出维度 (w, h)
  for (int i = 0; i < this->num_spatial_axes_; ++i) {
    // i + 1 to skip channel axis
    const int input_dim = this->input_shape(i + 1); // Feature Map 的输入维度
    const int kernel_extent = dilation_data[i] * (kernel_shape_data[i] - 1) + 1; // 扩展卷积核的维度
    const int output_dim = (input_dim + 2 * pad_data[i] - kernel_extent)
        / stride_data[i] + 1;
    this->output_shape_.push_back(output_dim);
  }
}

// 计算卷积前向操作
template <typename Dtype>
void ConvolutionLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  // 获取只读 weight 指针
  const Dtype* weight = this->blobs_[0]->cpu_data();
  for (int i = 0; i < bottom.size(); ++i) {
    // 获取只读 bottom_data 指针
    const Dtype* bottom_data = bottom[i]->cpu_data(); 
    // 获取读写 top_data 指针
    Dtype* top_data = top[i]->mutable_cpu_data();
    // num_ == batch size 依次对每一个 batch 进行操作
    for (int n = 0; n < this->num_; ++n) {
      // bottom_dim_  = 输入通道数c * 输入h * 输入w
      // top_dim_ = 输出通道数 * 输出h * 输出w
      this->forward_cpu_gemm(bottom_data + n * this->bottom_dim_, weight,
          top_data + n * this->top_dim_);
      if (this->bias_term_) {
        const Dtype* bias = this->blobs_[1]->cpu_data();
        this->forward_cpu_bias(top_data + n * this->top_dim_, bias);
      }
    }
  }
}

// 计算卷积反向操作
template <typename Dtype>
void ConvolutionLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  // 获取只读 weight 指针
  const Dtype* weight = this->blobs_[0]->cpu_data();
  // 获取读写 weight_diff 指针
  Dtype* weight_diff = this->blobs_[0]->mutable_cpu_diff();
  for (int i = 0; i < top.size(); ++i) { // 依次对 top 中的每一个 blob 进行操作
    // 获取只读 top_diff 指针
    const Dtype* top_diff = top[i]->cpu_diff();
    // 获取只读 bottom_data 指针
    const Dtype* bottom_data = bottom[i]->cpu_data();
    // 获取读写 bottom_diff 指针
    Dtype* bottom_diff = bottom[i]->mutable_cpu_diff();
    // Bias gradient, if necessary.
    if (this->bias_term_ && this->param_propagate_down_[1]) {
      // 获取读写 bias_diff 指针
      Dtype* bias_diff = this->blobs_[1]->mutable_cpu_diff();
      for (int n = 0; n < this->num_; ++n) {
        this->backward_cpu_bias(bias_diff, top_diff + n * this->top_dim_);
      }
    }
    if (this->param_propagate_down_[0] || propagate_down[i]) {
      for (int n = 0; n < this->num_; ++n) {    // 依次对每一个 batch 进行操作
        // gradient w.r.t. weight. Note that we will accumulate diffs.
        // 计算对 weight 的梯度: 这个地方的 top_diff 实际上就是激活函数回传的导数
        if (this->param_propagate_down_[0]) {
          this->weight_cpu_gemm(bottom_data + n * this->bottom_dim_,
              top_diff + n * this->top_dim_, weight_diff);
        }
        // gradient w.r.t. bottom data, if necessary. 
        // 这里的 bottom_diff 实际上就是下一层的 top_diff
        if (propagate_down[i]) {
          this->backward_cpu_gemm(top_diff + n * this->top_dim_, weight,
              bottom_diff + n * this->bottom_dim_);
        }
      }
    }
  }
}

#ifdef CPU_ONLY
STUB_GPU(ConvolutionLayer);
#endif

INSTANTIATE_CLASS(ConvolutionLayer);

}  // namespace caffe
