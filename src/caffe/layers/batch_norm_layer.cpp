#include <algorithm>
#include <vector>

#include "caffe/layers/batch_norm_layer.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

// 层设置函数
template <typename Dtype>
void BatchNormLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  // 读取层的参数
  BatchNormParameter param = this->layer_param_.batch_norm_param();
  // 获取滑动平均衰减系数
  moving_average_fraction_ = param.moving_average_fraction();
  // 对测试阶段 use_global_stats_ 默认为真
  use_global_stats_ = this->phase_ == TEST;
  if (param.has_use_global_stats())
    // 如果有 use_global_states 参数，则将修改 use_global_stats_
    use_global_stats_ = param.use_global_stats();
  if (bottom[0]->num_axes() == 1)
    // 如果 bottom blob 的轴数为 1，那么设置 channels_ 为 1
    channels_ = 1;
  else
    // 否则就正常读取 channels_ 值
    channels_ = bottom[0]->shape(1);
  // 读取分母附加项
  eps_ = param.eps();
  if (this->blobs_.size() > 0) {
    LOG(INFO) << "Skipping parameter initialization";
  } else {
    // 设置内部参数 blobs_ 的尺寸大小
    this->blobs_.resize(3);
    // 针对每个 channel 分别存储均值、方差和滑动平均衰减系数
    vector<int> sz;
    sz.push_back(channels_);
    // 设置均值的尺寸 (channels, )
    this->blobs_[0].reset(new Blob<Dtype>(sz));
    // 设置方差的尺寸 (channels, )
    this->blobs_[1].reset(new Blob<Dtype>(sz));
    sz[0] = 1;
    // 设置滑动平均衰减系数的尺寸 (1, )
    this->blobs_[2].reset(new Blob<Dtype>(sz));
    // 将该层的内部参数全部设置为 0
    for (int i = 0; i < 3; ++i) {
      caffe_set(this->blobs_[i]->count(), Dtype(0),
                this->blobs_[i]->mutable_cpu_data());
    }
  }
  // Mask statistics from optimization by setting local learning rates
  // for mean, variance, and the bias correction to zero.
  // 设置优化均值、方差和偏置的本地学习率
  for (int i = 0; i < this->blobs_.size(); ++i) {
    if (this->layer_param_.param_size() == i) {
      // 如果该层的 ParamSpec 参数数目不够，则添加参数，并且设置学习动量为 0         ??????
      ParamSpec* fixed_param_spec = this->layer_param_.add_param();
      fixed_param_spec->set_lr_mult(0.f);
    } else {
      CHECK_EQ(this->layer_param_.param(i).lr_mult(), 0.f)
          << "Cannot configure batch normalization statistics as layer "
          << "parameters.";
    }
  }
}

template <typename Dtype>
void BatchNormLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  // 检查 channel 个数是否合格
  if (bottom[0]->num_axes() >= 1)
    CHECK_EQ(bottom[0]->shape(1), channels_);
  // 设置 top blob 的形状大小
  top[0]->ReshapeLike(*bottom[0]);

  vector<int> sz;
  sz.push_back(channels_);
  // 设置均值的尺寸
  mean_.Reshape(sz);
  // 设置方差的尺寸
  variance_.Reshape(sz);
  // 设置中间变量的尺寸
  temp_.ReshapeLike(*bottom[0]);
  // 设置 x_norm_ 的尺寸
  x_norm_.ReshapeLike(*bottom[0]);
  sz[0] = bottom[0]->shape(0);
  // 设置 batch_sum_multiplier_ 的尺寸
  this->batch_sum_multiplier_.Reshape(sz);

  // 获取一张输入图片的 saptial_dim (height * width)
  int spatial_dim = bottom[0]->count()/(channels_*bottom[0]->shape(0));
  if (spatial_sum_multiplier_.num_axes() == 0 ||
      spatial_sum_multiplier_.shape(0) != spatial_dim) {
    sz[0] = spatial_dim;
    // 设置 spatial_sum_multiplier 的形状
    spatial_sum_multiplier_.Reshape(sz);
    // 获取读写 spatial_sum_multiplier_ 指针
    Dtype* multiplier_data = spatial_sum_multiplier_.mutable_cpu_data();
    // 将 spatial_sum_multiplier_ 所有元素全部设置为 1
    caffe_set(spatial_sum_multiplier_.count(), Dtype(1), multiplier_data);
  }

  // 设置通道的个数
  int numbychans = this->channels_*bottom[0]->shape(0);
  if (this->num_by_chans_.num_axes() == 0 ||
      this->num_by_chans_.shape(0) != numbychans) {
    sz[0] = numbychans;
    this->num_by_chans_.Reshape(sz);
    // 将 batch_sum_multiplier_ 的所有元素设置为 1
    caffe_set(this->batch_sum_multiplier_.count(), Dtype(1),
        this->batch_sum_multiplier_.mutable_cpu_data());
  }
}

template <typename Dtype>
void BatchNormLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  // 获取只读 bottom_data 的指针
  const Dtype* bottom_data = bottom[0]->cpu_data();
  // 获取读写 top_data 的指针
  Dtype* top_data = top[0]->mutable_cpu_data();
  // 获取 batch_size 的大小
  int num = bottom[0]->shape(0);
  int spatial_dim = bottom[0]->count()/(bottom[0]->shape(0) * this->channels_);

  // 如果输入 bottom 与输出 bottom 的地址不一致，
  // 则我们需要将 bottom_data 的数据拷贝到 top_data                             ??????
  if (bottom[0] != top[0]) {
    caffe_copy(bottom[0]->count(), bottom_data, top_data);
  }

  if (this->use_global_stats_) {
    // 如果 use_global_stats_ 为真，那么我们使用全局的均值和方差
    // use the stored mean/variance estimates.
    // 如果滑动平均系数为 0，设置 scale_factor 为 0，否则设置 scale_factor 为滑动平均系数的倒数 
    const Dtype scale_factor = this->blobs_[2]->cpu_data()[0] == 0 ?
        0 : 1 / this->blobs_[2]->cpu_data()[0];
    // 设置局部的均值                                                          ??????
    caffe_cpu_scale(this->variance_.count(), scale_factor,
        this->blobs_[0]->cpu_data(), this->mean_.mutable_cpu_data());
    // 设置局部方差
    caffe_cpu_scale(this->variance_.count(), scale_factor,
        this->blobs_[1]->cpu_data(), this->variance_.mutable_cpu_data());
  } else {
    // compute mean

    /*
    ** 函数：caffe_cpu_gemv<Dtype>(const CBLAS_TRANSPOSE TransA, const int M,  
                            const int N, const Dtype alpha, const Dtype* A, 
                            const Dtype* x, const Dtype beta, Dtype* y) 
    ** 功能：y = alpha * A * x + beta * y 
    ** 其中 x 和 y 是向量，A 是矩阵 
    ** M：A 的行数
    ** N：A 的列数  
    */ 
    // 数学表达式： num_by_chans_ = (1. / (num * spatial_dim)) * bottom_data * spatial_sum_multiplier_
    // bottom_data 是 (channels_ * num, spatial_dim)
    // spatial_sum_multiplier_ 是 (spatial_dim, 1)，元素全为 1
    // num_by_chans_ 是 (channels_ * num, 1)
    caffe_cpu_gemv<Dtype>(CblasNoTrans, this->channels_ * num, spatial_dim,
        1. / (num * spatial_dim), bottom_data,
        this->spatial_sum_multiplier_.cpu_data(), 0.,
        this->num_by_chans_.mutable_cpu_data());
    // 数学表达式：mean_ = Trans(num_by_chans) * batch_sum_multiplier
    // num_by_chans_ 是 (num, channels_)
    // batch_sum_multiplier_ 是 (num, 1)
    // mean_ 是 (channels_, 1)
    // 最终得到每个 channel 的平均值
    caffe_cpu_gemv<Dtype>(CblasTrans, num, this->channels_, 1.,
        this->num_by_chans_.cpu_data(), this->batch_sum_multiplier_.cpu_data(), 0.,
        this->mean_.mutable_cpu_data());
  }

  // subtract mean

  /*
    ** 函数：caffe_cpu_gemm<Dtype>(const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB, 
                            const int M, const int N, const int K, const Dtype alpha, 
                            const Dtype* A, const Dtype* B, const Dtype beta, Dtype* C)
    ** 功能：C = alpha * A * B + beta * C 
    ** 其中 A 是 (M, K)；B 是 (K, N)；C 是 (M, N)
    */
  // 数学表达式：num_by_chans_ = batch_sum_multiplier_ * mean_
  // batch_sum_multiplier_ 是 (num, 1)
  // mean_ 是 (1, channels_)
  // num_by_chans_ 是 (num, channels_)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, this->channels_, 1, 1,
      this->batch_sum_multiplier_.cpu_data(), this->mean_.cpu_data(), 0.,
      this->num_by_chans_.mutable_cpu_data());
  // 数学表达式：top_data = -1 * num_by_chans_ * spatial_sum_multiplier_ + top_data
  // num_by_chans_ 是 (channels_ * num, 1)
  // spatial_sum_multiplier_ 是 (1, spatial_dim)
  // top_data 是 (channels_ * num, spatial_dim)
  // 为每一个像素点减去均值
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, this->channels_ * num,
      spatial_dim, 1, -1, this->num_by_chans_.cpu_data(),
      this->spatial_sum_multiplier_.cpu_data(), 1., top_data);

  // 如果 use_global_stats_ 为真
  if (!this->use_global_stats_) {
    // compute variance using var(X) = E((X-EX)^2)
    // caffe_powx 是 element-wise 操作，这里实现对每个元素平方
    caffe_powx(top[0]->count(), top_data, Dtype(2),
        this->temp_.mutable_cpu_data());  // (X-EX)^2
    
    // 数学表达式：num_by_chans_ = 1. / (num * spatial_dim) * temp_ * spatial_sum_multiplier_
    // temp_ 是 (channels_ * num, spatial_dim)
    // spatial_sum_multiplier_ 是 (spatial_dim, 1)，元素全为 1
    // num_by_chans_ 是 (channels_ * num, 1)
    caffe_cpu_gemv<Dtype>(CblasNoTrans, this->channels_ * num, spatial_dim,
        1. / (num * spatial_dim), this->temp_.cpu_data(),
        this->spatial_sum_multiplier_.cpu_data(), 0.,
        this->num_by_chans_.mutable_cpu_data());
    
    // 数学表达式：variance_ = Trans(num_by_chans_) * batch_sum_multiplier_
    // num_by_chans 是 (num, channels_)
    // batch_sum_multiplier_ 是 (num, 1)，元素全为 1
    // varaince_ 是 (channels_, 1)
    // 计算出方差
    caffe_cpu_gemv<Dtype>(CblasTrans, num, this->channels_, 1.,
        this->num_by_chans_.cpu_data(), this->batch_sum_multiplier_.cpu_data(), 0.,
        this->variance_.mutable_cpu_data());  // E((X_EX)^2)

    // compute and save moving average
    this->blobs_[2]->mutable_cpu_data()[0] *= this->moving_average_fraction_;
    this->blobs_[2]->mutable_cpu_data()[0] += 1;
    /*
    ** caffe_cpu_axpby<Dtype>(const int N, const Dtype alpha, const Dtype* X,  
                              const Dtype beta, Dtype* Y)
    ** 功能：Y= alpha * X + beta * Y
    ** X 是 (N, 1)
    ** Y 是 (N, 1)
    */

    // 数学表达式：blobs_[0] = mean_ + moving_average_fraction_ * blobs_[0]  
    caffe_cpu_axpby(this->mean_.count(), Dtype(1), this->mean_.cpu_data(),
        this->moving_average_fraction_, this->blobs_[0]->mutable_cpu_data());
    int m = bottom[0]->count()/channels_;
    // 计算无偏差估计的系数 m/(m - 1)
    Dtype bias_correction_factor = m > 1 ? Dtype(m)/(m-1) : 1;
    // 数学表达式：blobs_[1] = bias_correction_factor * variance_ + moving_average_fraction_ * blobs_[1]
    caffe_cpu_axpby(variance_.count(), bias_correction_factor,
        variance_.cpu_data(), moving_average_fraction_,
        this->blobs_[1]->mutable_cpu_data());
  }

  // normalize variance
  /*
  ** caffe_add_scalar(const int N, const float alpha, float* Y)
  ** 功能：给 Y 的每个元素加上 alpha
  */
  // 为方差加上一个附加值，防止除数为 0
  caffe_add_scalar(this->variance_.count(), this->eps_, this->variance_.mutable_cpu_data());
  // 为每个元素求开平方
  caffe_powx(this->variance_.count(), this->variance_.cpu_data(), Dtype(0.5),
             this->variance_.mutable_cpu_data());

  // replicate variance to input size
  // 数学表达式：num_by_chans_ = batch_sum_multiplier_ * variance_
  // batch_sum_multiplier_ 是 (num, 1)，元素全为 1
  // variance_ 是 (1, channels)
  // num_by_chans 是 (num, channels)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, this->channels_, 1, 1,
      this->batch_sum_multiplier_.cpu_data(), this->variance_.cpu_data(), 0.,
      this->num_by_chans_.mutable_cpu_data());

  // 数学表达式：temp_ = num_by_chans_ * spatial_sum_multiplier
  // num_by_chans_ 是 (channels * num, 1)
  // spatial_sum_multiplier_ 是 (1, spatial_dim)，元素全为 1
  // temp_ 是 (channels * num, spatial_dim)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, this->channels_ * num,
      spatial_dim, 1, 1., this->num_by_chans_.cpu_data(),
      this->spatial_sum_multiplier_.cpu_data(), 0., this->temp_.mutable_cpu_data());
  // 实现 element-wise 相除
  caffe_div(this->temp_.count(), top_data, this->djfde.cpu_data(), top_data);
  // TODO(cdoersch): The caching is only needed because later in-place layers
  //                 might clobber the data.  Can we skip this if they won't?
  caffe_copy(this->x_norm_.count(), top_data, this->x_norm_.mutable_cpu_data());
}

template <typename Dtype>
void BatchNormLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {
  // 获取只读 top_diff 指针
  const Dtype* top_diff;
  if (bottom[0] != top[0]) { // 如果 bottom[0] 与 top[0] 所指的地址不一致
    top_diff = top[0]->cpu_diff();
  } else {
    caffe_copy(this->x_norm_.count(), top[0]->cpu_diff(), this->x_norm_.mutable_cpu_diff());
    top_diff = this->x_norm_.cpu_diff();
  }
  // 获取读写 bottom_diff 的指针
  Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();
  // 如果 use_global_stats_ 为真
  if (this->use_global_stats_) {
    caffe_div(this->temp_.count(), top_diff, this->temp_.cpu_data(), bottom_diff);
    return;
  }
  // 获取只读 x_norm_ 指针
  const Dtype* top_data = this->x_norm_.cpu_data();
  // 获取 batch_size 的大小
  int num = bottom[0]->shape()[0];
  // 获取 spatial_dim 的大小
  int spatial_dim = bottom[0]->count()/(bottom[0]->shape(0) * this->channels_);
  // if Y = (X-mean(X))/(sqrt(var(X)+eps)), then
  //
  // dE(Y)/dX =
  //   (dE/dY - mean(dE/dY) - mean(dE/dY \cdot Y) \cdot Y)
  //     ./ sqrt(var(X) + eps)
  //
  // where \cdot and ./ are hadamard product and elementwise division,
  // respectively, dE/dY is the top diff, and mean/var/sum are all computed
  // along all dimensions except the channels dimension.  In the above
  // equation, the operations allow for expansion (i.e. broadcast) along all
  // dimensions except the channels dimension where required.

  // sum(dE/dY \cdot Y)

  // 实现 element wise 相乘 
  // bottom_diff[i] = top_data[i] * top_diff[i]
  caffe_mul(this->temp_.count(), top_data, top_diff, bottom_diff);

  // 数学表达式：num_by_chans_ = bottom_diff * spatial_sum_multiplier_
  // bottom_diff 是 (channels_ * num, spatial_dim)
  // spatial_sum_multiplier_ 是 (spatial_dim, 1)
  // num_by_chans 是 (channels_ * num, 1)
  caffe_cpu_gemv<Dtype>(CblasNoTrans, this->channels_ * num, spatial_dim, 1.,
      bottom_diff, this->spatial_sum_multiplier_.cpu_data(), 0.,
      this->num_by_chans_.mutable_cpu_data());

  // 数学表达式：mean_ = Trans(num_by_chans_) * batch_sum_multiplier_
  // num_by_chans_ 是 (num, channels)
  // batch_sum_multiplier_ 是 (num, 1)，元素全是 1
  // mean_ 是 (channels, 1) 
  caffe_cpu_gemv<Dtype>(CblasTrans, num, this->channels_, 1.,
      this->num_by_chans_.cpu_data(), this->batch_sum_multiplier_.cpu_data(), 0.,
      this->mean_.mutable_cpu_data());

  // reshape (broadcast) the above
  // 数学表达式：num_by_chans_ = batch_sum_multiplier_ * mean_
  // batch_sum_multiplier_ 是 (num, 1)，元素全是 1
  // mean_ 是 (1, channels_)
  // num_by_chans_ 是 (num, channels_)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, this->channels_, 1, 1,
      this->batch_sum_multiplier_.cpu_data(), this->mean_.cpu_data(), 0.,
      this->num_by_chans_.mutable_cpu_data());
  
  // 数学表达式：bottom_diff = num_by_chans_ * saptial_sum_multiplier_
  // num_by_chans_ 是 (channels * num, 1)
  // spatial_sum_multiplier_ 是 (1, spatial_dim)，所有元素为 1
  // bottom_diff 是 (channels * num, spatial_dim)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, this->channels_ * num,
      spatial_dim, 1, 1., this->num_by_chans_.cpu_data(),
      this->spatial_sum_multiplier_.cpu_data(), 0., bottom_diff);

  // sum(dE/dY \cdot Y) \cdot Y
  // 采用 element-wise 的乘法
  // bottom_diff[i] = top_data[i] * bottom_diff[i]
  caffe_mul(this->temp_.count(), top_data, bottom_diff, bottom_diff);

  // sum(dE/dY)-sum(dE/dY \cdot Y) \cdot Y
  // 数学表达式：num_by_chans_ = top_diff * spatial_sum_multiplier_ 
  // top_diff 是 (channels_ * num, spatial_dim)
  // spatial_sum_multiplier_ 是 (spatial_dim, 1)，所有元素为 1
  // num_by_chans_ 是 (channels_ * num, 1) 
  caffe_cpu_gemv<Dtype>(CblasNoTrans, this->channels_ * num, spatial_dim, 1.,
      top_diff, this->spatial_sum_multiplier_.cpu_data(), 0.,
      this->num_by_chans_.mutable_cpu_data());

  // 数学表达式：mean_ = Trans(num_by_chans_) * batch_sum_multiplier_
  // num_by_chans 是 (num, channels_)
  // batch_sum_multiplier_ 是 (num, 1)，所有元素为 1
  // mean_ 是 (channels_, 1)
  caffe_cpu_gemv<Dtype>(CblasTrans, num, this->channels_, 1.,
      this->num_by_chans_.cpu_data(), this->batch_sum_multiplier_.cpu_data(), 0.,
      this->mean_.mutable_cpu_data());
  
  // reshape (broadcast) the above to make
  // sum(dE/dY)-sum(dE/dY \cdot Y) \cdot Y
  // 数学表达式：num_by_chans_ = batch_sum_multiplier_ * mean_
  // batch_sum_multiplier_ 是 (num, 1)，元素全为 1
  // mean_ 是 (1, channels)
  // num_by_chans_ 是 (num, channels)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, this->channels_, 1, 1,
      this->batch_sum_multiplier_.cpu_data(), this->mean_.cpu_data(), 0.,
      this->num_by_chans_.mutable_cpu_data());

  // 数学表达式：bottom_diff = num_by_chans_ * spatial_sum_multiplier_
  // num_by_chans_ 是 (num * channels_, 1)
  // spatial_sum_multiplier_ 是 (1, spatial_dim)，元素全为 1
  // bottom_diff 是 (num * channels_, spatial_dim)
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num * channels_,
      spatial_dim, 1, 1., num_by_chans_.cpu_data(),
      spatial_sum_multiplier_.cpu_data(), 1., bottom_diff);

  // dE/dY - mean(dE/dY)-mean(dE/dY \cdot Y) \cdot Y

  /*
    ** caffe_cpu_axpby<Dtype>(const int N, const Dtype alpha, const Dtype* X,  
                              const Dtype beta, Dtype* Y)
    ** 功能：Y= alpha * X + beta * Y
    ** X 是 (N, 1)
    ** Y 是 (N, 1)
    */
  // 数学表达式：bottom_diff = top_diff - 1 / (num * spatial_dim) * bottom_diff
  caffe_cpu_axpby(temp_.count(), Dtype(1), top_diff,
      Dtype(-1. / (num * spatial_dim)), bottom_diff);

  // note: temp_ still contains sqrt(var(X)+eps), computed during the forward
  // pass.
  // 实现 element-wise 元素相除
  caffe_div(temp_.count(), bottom_diff, temp_.cpu_data(), bottom_diff);
}


#ifdef CPU_ONLY
STUB_GPU(BatchNormLayer);
#endif

INSTANTIATE_CLASS(BatchNormLayer);
REGISTER_LAYER_CLASS(BatchNorm);
}  // namespace caffe
