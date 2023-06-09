#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "Timer.h"
#include "context.h"
#include "cublas_wrappers.h"
#include "custom_cuda_layers.h"
#include "ds_transformer_cuda.h"

//#define PERF_TEST
#include <iostream>
using namespace std;
static std::unordered_map<int, std::shared_ptr<void>> s_LayerNormalize_layers;
static std::unordered_map<int, std::shared_ptr<void>> s_transformer_layers;
static std::unordered_map<int, std::shared_ptr<void>> s_bias_residual_dropout_layers;
static std::unordered_map<int, std::shared_ptr<void>> s_mlp_layers;
static std::unordered_map<int, std::shared_ptr<void>> s_self_attention_layers;

// NOTE: AT_ASSERT has become AT_CHECK on master after 0.4.
#define CHECK_CUDA(x) AT_ASSERTM(x.type().is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) \
    CHECK_CUDA(x);     \
    CHECK_CONTIGUOUS(x)

// C++ interface

template <typename T>
size_t get_workspace_size(int maxBatchSize,
                          int seq_len,
                          int hidden_size,
                          int heads,
                          bool training,
                          bool gelu_checkpoint)
{
    size_t workSpacesize = 4 * (size_t(maxBatchSize) * seq_len * hidden_size);
    if (training) {
        workSpacesize += (std::max((4 * size_t(maxBatchSize) * seq_len * hidden_size),
                                   2 * (size_t(maxBatchSize) * heads * seq_len * seq_len)));
        if (gelu_checkpoint) workSpacesize += 2 * (size_t(maxBatchSize) * seq_len * hidden_size);
    }
    return workSpacesize * sizeof(T);
}


template <typename T>
size_t get_workspace_size(int maxBatchSize,
                          int seq_len,
                          int hidden_size,
                          int heads,
                          bool training)
{
    size_t workSpacesize = 4 * (size_t(maxBatchSize) * seq_len * hidden_size);
    if (training) {
        workSpacesize += (std::max((4 * size_t(maxBatchSize) * seq_len * hidden_size),
                                   2 * (size_t(maxBatchSize) * heads * seq_len * seq_len)));
    }
    return workSpacesize * sizeof(T);
}

// C++ interface


// NOTE: AT_ASSERT has become AT_CHECK on master after 0.4.
#define CHECK_CUDA(x) AT_ASSERTM(x.type().is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) \
    CHECK_CUDA(x);     \
    CHECK_CONTIGUOUS(x)

template <typename T>
BertTransformerLayer<T>::BertTransformerLayer(int layer_id,
                                              int batch_size,
                                              int hidden_size,
                                              int num_heads,
                                              int intermediate_size,
                                              int seq_length,
                                              float attn_prob_dropout_ratio,
                                              float hidden_output_dropout_ratio,
                                              bool pre_or_postLayerNorm,
                                              const std::vector<std::array<int, 3>>& gemm_algos,
                                              bool attn_dropout_checkpoint,
                                              bool normalize_invertible,
                                              bool gelu_checkpoint,
                                              bool stochastic_mode)
    : _layer_id(layer_id),
      _batch_size(batch_size),
      _hidden_size(hidden_size),
      _heads(num_heads),
      _intermediate_size(intermediate_size),
      _seq_length(seq_length),
      _training(true),
      _pre_or_postLayerNorm(pre_or_postLayerNorm),
      _attn_dropout_checkpoint(attn_dropout_checkpoint),
      _normalize_invertible(normalize_invertible),
      _gelu_checkpoint(gelu_checkpoint),
      _stochastic_mode(stochastic_mode),
      _stream(Context::Instance().GetCurrentStream()),
      _cublasHandle(Context::Instance().GetCublasHandle()),
      _qkv_linear(typename FeedForward<T>::Config(batch_size * seq_length,
                                                  3 * hidden_size,
                                                  hidden_size,
                                                  gemm_algos[0])),
      _attn_out_linear(typename FeedForward<T>::Config(batch_size * seq_length,
                                                       hidden_size,
                                                       hidden_size,
                                                       gemm_algos[0])),
      _norm_layer2(typename Normalize_Layer<T>::Config(batch_size,
                                                       seq_length,
                                                       hidden_size,
                                                       true,
                                                       false,
                                                       false,
                                                       !normalize_invertible)),
      _norm_layer3(typename Normalize_Layer<T>::Config(batch_size,
                                                       seq_length,
                                                       hidden_size,
                                                       true,
                                                       false,
                                                       false,
                                                       !normalize_invertible)),
      _ff1(typename FeedForward<T>::Config(batch_size * seq_length,
                                           4 * hidden_size,
                                           hidden_size,
                                           gemm_algos[1])),
      _ff2(typename FeedForward<T>::Config(batch_size * seq_length,
                                           hidden_size,
                                           4 * hidden_size,
                                           gemm_algos[2])),
      _softmax(typename Softmax<T>::Config(batch_size, num_heads, seq_length)),
      _gelu(typename Gelu<T>::Config(_batch_size, _seq_length, _intermediate_size)),
      _attn_prob_dropout(typename Dropout<T>::Config(attn_prob_dropout_ratio,
                                                     _batch_size * _heads * _seq_length,
                                                     _seq_length)),
      _attn_output_dropout(typename Dropout<T>::Config(hidden_output_dropout_ratio,
                                                       _batch_size * _seq_length,
                                                       _hidden_size)),
      _layer_output_dropout(typename Dropout<T>::Config(hidden_output_dropout_ratio,
                                                        _batch_size * _seq_length,
                                                        _hidden_size)),
      _attn_scores(typename StridedBatchGemm<T>::Config(_batch_size * _heads,
                                                        _seq_length,
                                                        _seq_length,
                                                        _hidden_size / _heads,
                                                        (T(1.0) / T(sqrt(_hidden_size / _heads))),
                                                        T(0.0),
                                                        CUBLAS_OP_T,
                                                        CUBLAS_OP_N,
                                                        gemm_algos[3])),
      _attn_context(typename StridedBatchGemm<T>::Config(_batch_size * _heads,
                                                         _hidden_size / _heads,
                                                         _seq_length,
                                                         _seq_length,
                                                         T(1.0),
                                                         T(0.0),
                                                         CUBLAS_OP_N,
                                                         CUBLAS_OP_N,
                                                         gemm_algos[4]))
{
    assert(_hidden_size % _heads == 0);
    assert(_seq_length <= 1024);

    Initialize();
}

template <typename T>
BertTransformerLayer<T>::~BertTransformerLayer()
{
}

template <typename T>
void BertTransformerLayer<T>::Initialize()
{
    Context::Instance().GenWorkSpace(get_workspace_size<T>(
        _batch_size, _seq_length, _hidden_size, _heads, _training, _gelu_checkpoint));

    if (std::is_same<T, __half>::value) cublasSetMathMode(_cublasHandle, CUBLAS_TENSOR_OP_MATH);
}

template <typename T>
void BertTransformerLayer<T>::Forward(int bsz,
                                      const T* input_ptr,
                                      const T* input_mask_ptr,
                                      const T* attn_qkvw_ptr,
                                      const T* attn_qkvb_ptr,
                                      const T* attn_ow_ptr,
                                      const T* attn_ob_ptr,
                                      const T* attn_nw_ptr,
                                      const T* attn_nb_ptr,
                                      const T* inter_w_ptr,
                                      const T* inter_b_ptr,
                                      const T* output_w_ptr,
                                      const T* output_b_ptr,
                                      const T* norm_w_ptr,
                                      const T* norm_b_ptr,
                                      T* out_ptr,
                                      T* inp_norm_ptr,
                                      T* q_tf_ptr,
                                      T* k_tf_ptr,
                                      T* v_tf_ptr,
                                      T* soft_out_ptr,
                                      T* ctx_bufB_ptr,
                                      T* attn_o_inp_ptr,
                                      T* add_res_ptr,
                                      T* ff1_inp_ptr,
                                      T* gelu_inp_ptr,
                                      T* ff2_inp_ptr)
{
    cublasSetStream(_cublasHandle, _stream);

    if (!_stochastic_mode) cudaStreamSynchronize(_stream);

    T* workspace = static_cast<T*>(Context::Instance().GetWorkSpace());
    size_t small_buf_size = bsz * _seq_length * _hidden_size;
    T* buf_0 = workspace;
    T* buf_1 = buf_0 + small_buf_size;

    if (_normalize_invertible) add_res_ptr = buf_1 + 3 * small_buf_size;
    if (_attn_dropout_checkpoint) ctx_bufB_ptr = buf_1 + 4 * small_buf_size;

    if (_pre_or_postLayerNorm) {
        if (_norm_layer3.UseMean())
            _norm_layer3.ForwardCheckpoint(
                bsz, inp_norm_ptr, input_ptr, norm_w_ptr, norm_b_ptr, _stream, true);

        else
            _norm_layer3.Forward(
                bsz, inp_norm_ptr, input_ptr, norm_w_ptr, norm_b_ptr, _stream, true);
    }

    int bsz_seq = bsz * _seq_length;

    if (_pre_or_postLayerNorm)
        _qkv_linear.Forward(bsz_seq, inp_norm_ptr, attn_qkvw_ptr, buf_0, _cublasHandle);
    else
        _qkv_linear.Forward(bsz_seq, input_ptr, attn_qkvw_ptr, buf_0, _cublasHandle);

    launch_bias_add_transform_0213<T>(
        q_tf_ptr, buf_0, attn_qkvb_ptr, bsz, _seq_length, _hidden_size, _heads, _stream, 3);

    int bsz_heads = bsz * _heads;

    // attention scores
    _attn_scores.Forward(bsz_heads, soft_out_ptr, k_tf_ptr, q_tf_ptr, _cublasHandle);

    // Softmax + Mask
    _softmax.Forward(bsz, soft_out_ptr, input_mask_ptr, _stream);

    // attn prob dropout.
    _attn_prob_dropout.Forward(bsz_heads * _seq_length, ctx_bufB_ptr, soft_out_ptr, _stream);

    //_softmax.Forward_fused_dropout(bsz, soft_out_ptr, input_mask_ptr, 
    //            _attn_prob_dropout.GetMask(), _attn_prob_dropout.GetRatio(), _stream, 512);

    // attention context
    _attn_context.Forward(bsz_heads, buf_1, v_tf_ptr, ctx_bufB_ptr, _cublasHandle);

    launch_transform4d_0213<T>(
        attn_o_inp_ptr, buf_1, bsz, _heads, _seq_length, _hidden_size, _stream, 1);

    if (_pre_or_postLayerNorm)
        _attn_out_linear.Forward(bsz_seq, attn_o_inp_ptr, attn_ow_ptr, buf_1, _cublasHandle);
    else
        _attn_out_linear.Forward(bsz_seq, attn_o_inp_ptr, attn_ow_ptr, ff1_inp_ptr, _cublasHandle);

    // attn output dropout.
    if (_pre_or_postLayerNorm)
        _attn_output_dropout.ForwardWithBias(
            bsz_seq, add_res_ptr, buf_1, input_ptr, attn_ob_ptr, _stream);
    else
        _attn_output_dropout.ForwardWithBias(
            bsz_seq, add_res_ptr, ff1_inp_ptr, input_ptr, attn_ob_ptr, _stream);

    if (_pre_or_postLayerNorm) {
        if (_norm_layer2.UseMean())
            _norm_layer2.ForwardCheckpoint(
                bsz, ff1_inp_ptr, add_res_ptr, attn_nw_ptr, attn_nb_ptr, _stream, true);
        else
            _norm_layer2.Forward(
                bsz, ff1_inp_ptr, add_res_ptr, attn_nw_ptr, attn_nb_ptr, _stream, true);
    } else {
        if (_norm_layer2.UseMean())
            _norm_layer2.ForwardCheckpoint(
                bsz, ff1_inp_ptr, add_res_ptr, attn_nw_ptr, attn_nb_ptr, _stream, true);
        else
            _norm_layer2.Forward(
                bsz, ff1_inp_ptr, add_res_ptr, attn_nw_ptr, attn_nb_ptr, _stream, true);
    }

    _ff1.Forward(bsz_seq,
                 ff1_inp_ptr,
                 inter_w_ptr,
                 (_gelu_checkpoint ? ff2_inp_ptr : gelu_inp_ptr),
                 _cublasHandle);

    _gelu.ForwardWithBiasAdd(bsz,
                             (_gelu_checkpoint ? ff2_inp_ptr : gelu_inp_ptr),
                             inter_b_ptr,
                             (_gelu_checkpoint ? ctx_bufB_ptr : ff2_inp_ptr),
                             _stream);

    _ff2.Forward(bsz_seq,
                 (_gelu_checkpoint ? ctx_bufB_ptr : ff2_inp_ptr),
                 output_w_ptr,
                 out_ptr,
                 _cublasHandle);

    // layer output dropout.
    if (_pre_or_postLayerNorm)
        _layer_output_dropout.ForwardWithBias(
            bsz_seq, out_ptr, out_ptr, add_res_ptr, output_b_ptr, _stream);
    else
        _layer_output_dropout.ForwardWithBias(
            bsz_seq, inp_norm_ptr, out_ptr, ff1_inp_ptr, output_b_ptr, _stream);

    if (!_pre_or_postLayerNorm) {
        if (_norm_layer3.UseMean())
            _norm_layer3.ForwardCheckpoint(
                bsz, out_ptr, inp_norm_ptr, norm_w_ptr, norm_b_ptr, _stream, true);
        else
            _norm_layer3.Forward(bsz, out_ptr, inp_norm_ptr, norm_w_ptr, norm_b_ptr, _stream, true);
    }
}

template <typename T>
void BertTransformerLayer<T>::Backward(int bsz,
                                       const T* grad_output_ptr,
                                       const T* input_ptr,
                                       const T* output_ptr,
                                       const T* inp_norm_ptr,
                                       const T* q_tf_ptr,
                                       const T* k_tf_ptr,
                                       const T* v_tf_ptr,
                                       const T* soft_out_ptr,
                                       const T* ctx_bufB_ptr,
                                       const T* attn_o_inp_ptr,
                                       const T* add_res_ptr,
                                       const T* ff1_inp_ptr,
                                       const T* gelu_inp_ptr,
                                       const T* ff2_inp_ptr,
                                       const T* input_mask_ptr,
                                       const T* attn_qkvw_ptr,
                                       const T* attn_ow_ptr,
                                       const T* attn_nw_ptr,
                                       const T* attn_nb_ptr,
                                       const T* inter_w_ptr,
                                       const T* inter_b_ptr,
                                       const T* output_w_ptr,
                                       const T* norm_w_ptr,
                                       const T* norm_b_ptr,

                                       T* grad_input_ptr,
                                       T* grad_attn_qkvw_ptr,
                                       T* grad_attn_qkvb_ptr,
                                       T* grad_attn_ow_ptr,
                                       T* grad_attn_ob_ptr,
                                       T* grad_attn_nw_ptr,
                                       T* grad_attn_nb_ptr,
                                       T* grad_inter_w_ptr,
                                       T* grad_inter_b_ptr,
                                       T* grad_output_w_ptr,
                                       T* grad_output_b_ptr,
                                       T* grad_norm_w_ptr,
                                       T* grad_norm_b_ptr)
{
    cublasSetStream(_cublasHandle, _stream);

    if (!_stochastic_mode) cudaStreamSynchronize(_stream);

    T* workspace = static_cast<T*>(Context::Instance().GetWorkSpace());
    size_t small_buf_size = bsz * _seq_length * _hidden_size;
    T* buf_0 = workspace;
    T* buf_1 = buf_0 + small_buf_size;
    T* buf_2 = buf_1 + small_buf_size;
    T* buf_3 = buf_2 + small_buf_size;

    T* ff2_buf = buf_3 + (_gelu_checkpoint ? 3 : 1) * small_buf_size;
    T* ctx_bufB_ptr_recomp = ff2_buf + (_seq_length * _seq_length * bsz * _heads);

    cudaStream_t streams[2] = {_stream, _stream};

    int bsz_seq = bsz * _seq_length;
    int bsz_heads = bsz * _heads;

    if (!_pre_or_postLayerNorm) {
        if (_norm_layer3.UseMean())
            _norm_layer3.Backward(bsz,
                                  grad_output_ptr,
                                  norm_w_ptr,
                                  grad_norm_w_ptr,
                                  grad_norm_b_ptr,
                                  streams,
                                  buf_1,
                                  inp_norm_ptr);

        else
            _norm_layer3.Backward(bsz,
                                  grad_output_ptr,
                                  norm_w_ptr,
                                  norm_b_ptr,
                                  grad_norm_w_ptr,
                                  grad_norm_b_ptr,
                                  streams,
                                  buf_1,
                                  output_ptr);
    }

    if (_pre_or_postLayerNorm)
        _layer_output_dropout.Backward(bsz_seq, buf_0, grad_output_ptr, _stream);
    else
        _layer_output_dropout.Backward(bsz_seq, buf_0, buf_1, _stream);

    const T* layer_dropout_buf = _layer_output_dropout.HasDropout()
                                     ? buf_0
                                     : (_pre_or_postLayerNorm ? grad_output_ptr : buf_1);

    if (_gelu_checkpoint) _gelu.ForwardWithBiasAdd(bsz, ff2_inp_ptr, inter_b_ptr, buf_2, _stream);
    _ff2.Backward(bsz_seq,
                  layer_dropout_buf,
                  (_gelu_checkpoint ? buf_2 : ff2_inp_ptr),
                  output_w_ptr,
                  grad_output_w_ptr,
                  grad_output_b_ptr,
                  _cublasHandle,
                  _stream,
                  ff2_buf);

    _gelu.Backward(
        bsz, ff2_buf, (_gelu_checkpoint ? ff2_inp_ptr : gelu_inp_ptr), inter_b_ptr, _stream);

    _ff1.Backward(bsz_seq,
                  ff2_buf,
                  ff1_inp_ptr,
                  inter_w_ptr,
                  grad_inter_w_ptr,
                  grad_inter_b_ptr,
                  _cublasHandle,
                  _stream,
                  buf_3);

    if (!_pre_or_postLayerNorm)
        launch_fused_add2<T>(buf_2, buf_3, buf_1, bsz, _seq_length, _hidden_size, _stream);

    if (_pre_or_postLayerNorm) {
        if (_norm_layer2.UseMean())
            _norm_layer2.BackwardFusedAdd(bsz,
                                          buf_3,
                                          grad_output_ptr,
                                          attn_nw_ptr,
                                          grad_attn_nw_ptr,
                                          grad_attn_nb_ptr,
                                          streams,
                                          buf_0,
                                          add_res_ptr);

        else
            _norm_layer2.BackwardFusedAdd(bsz,
                                          buf_3,
                                          grad_output_ptr,
                                          attn_nw_ptr,
                                          attn_nb_ptr,
                                          grad_attn_nw_ptr,
                                          grad_attn_nb_ptr,
                                          streams,
                                          buf_0,
                                          ff1_inp_ptr);
    } else {
        if (_norm_layer2.UseMean())
            _norm_layer2.Backward(bsz,
                                  buf_2,
                                  attn_nw_ptr,
                                  grad_attn_nw_ptr,
                                  grad_attn_nb_ptr,
                                  streams,
                                  buf_0,
                                  add_res_ptr);

        else
            _norm_layer2.Backward(bsz,
                                  buf_2,
                                  attn_nw_ptr,
                                  attn_nb_ptr,
                                  grad_attn_nw_ptr,
                                  grad_attn_nb_ptr,
                                  streams,
                                  buf_0,
                                  ff1_inp_ptr);
    }

    _attn_output_dropout.Backward(bsz_seq, buf_2, buf_0, _stream);

    T* attn_output_dropout_buf = _attn_output_dropout.HasDropout() ? buf_2 : buf_0;

    _attn_out_linear.Backward(bsz_seq,
                              attn_output_dropout_buf,
                              attn_o_inp_ptr,
                              attn_ow_ptr,
                              grad_attn_ow_ptr,
                              grad_attn_ob_ptr,
                              _cublasHandle,
                              _stream,
                              buf_1);

    launch_transform_0213<T>(buf_2, buf_1, bsz, _seq_length, _hidden_size, _heads, _stream);

    if (_attn_prob_dropout.HasDropout()) {
        if (_attn_dropout_checkpoint)
            _attn_prob_dropout.Forward(
                bsz_heads * _seq_length, ctx_bufB_ptr_recomp, soft_out_ptr, _stream, true);

        _attn_context.Backward(bsz_heads,
                               buf_2,
                               v_tf_ptr,
                               (_attn_dropout_checkpoint ? ctx_bufB_ptr_recomp : ctx_bufB_ptr),
                               _cublasHandle,
                               buf_3,
                               ff2_buf);
    } else
        _attn_context.Backward(
            bsz_heads, buf_2, v_tf_ptr, soft_out_ptr, _cublasHandle, buf_3, ff2_buf);

    _attn_prob_dropout.Backward(bsz_heads * _seq_length, ff2_buf, _stream);

    _softmax.Backward(bsz, ff2_buf, soft_out_ptr, _stream);

    //_softmax.Backward_fused_dropout(bsz, ff2_buf, soft_out_ptr, 
      //      _attn_prob_dropout.GetMask(), _attn_prob_dropout.GetRatio(), _stream, 1024);

    _attn_scores.Backward(bsz_heads, ff2_buf, k_tf_ptr, q_tf_ptr, _cublasHandle, buf_2, buf_1);

    launch_transform4d_0213(ff2_buf, buf_1, bsz, _heads, _seq_length, _hidden_size, _stream, 3);

    if (_pre_or_postLayerNorm)
        _qkv_linear.Backward(bsz_seq,
                             ff2_buf,
                             inp_norm_ptr,
                             attn_qkvw_ptr,
                             grad_attn_qkvw_ptr,
                             grad_attn_qkvb_ptr,
                             _cublasHandle,
                             _stream,
                             buf_2);
    else
        _qkv_linear.Backward(bsz_seq,
                             ff2_buf,
                             input_ptr,
                             attn_qkvw_ptr,
                             grad_attn_qkvw_ptr,
                             grad_attn_qkvb_ptr,
                             _cublasHandle,
                             _stream,
                             buf_2);

    if (_pre_or_postLayerNorm) {
        if (_norm_layer3.UseMean())
            _norm_layer3.BackwardFusedAdd(bsz,
                                          buf_2,
                                          buf_0,
                                          norm_w_ptr,
                                          grad_norm_w_ptr,
                                          grad_norm_b_ptr,
                                          streams,
                                          grad_input_ptr,
                                          input_ptr);

        else
            _norm_layer3.BackwardFusedAdd(bsz,
                                          buf_2,
                                          buf_0,
                                          norm_w_ptr,
                                          norm_b_ptr,
                                          grad_norm_w_ptr,
                                          grad_norm_b_ptr,
                                          streams,
                                          grad_input_ptr,
                                          inp_norm_ptr);
    } else
        launch_fused_add2<T>(grad_input_ptr, buf_2, buf_0, bsz, _seq_length, _hidden_size, _stream);
}

template <typename T>
void BertTransformerLayer<T>::SetTrainingMode(bool training)
{
    // Dropout will be skipped when not in training model.
    _attn_prob_dropout.SetTrainingMode(training);
    _attn_output_dropout.SetTrainingMode(training);
    _layer_output_dropout.SetTrainingMode(training);
}

template <typename T>
void BertTransformerLayer<T>::SetIntermediateBuffers(uint8_t* attn_prob_dropout_mask_ptr,
                                                     uint8_t* attn_output_dropout_mask_ptr,
                                                     uint8_t* layer_output_dropout_mask_ptr)
{
    _attn_prob_dropout.SetMask(attn_prob_dropout_mask_ptr);
    _attn_output_dropout.SetMask(attn_output_dropout_mask_ptr);
    _layer_output_dropout.SetMask(layer_output_dropout_mask_ptr);
}



template <typename T>
int create_transformer_layer(int layer_id,
                             int batch_size,
                             int hidden_dim,
                             int num_heads,
                             int intermediate_size,
                             int seq_length,
                             float attn_dropout_ratio,
                             float hidden_dropout_ratio,
                             int seed,
                             bool pre_or_postLayerNorm,
                             bool test_gemm,
                             bool attn_dropout_checkpoint,
                             bool normalize_invertible,
                             bool gelu_checkpoint,
                             bool stochastic_mode)
{
    Context::Instance().SetSeed(seed);
    Context::Instance().TestGemmFP16(
        test_gemm, batch_size, seq_length, num_heads, hidden_dim / num_heads);

    auto layer = std::make_shared<BertTransformerLayer<T>>(layer_id,
                                                           batch_size,
                                                           hidden_dim,
                                                           num_heads,
                                                           intermediate_size,
                                                           seq_length,
                                                           attn_dropout_ratio,
                                                           hidden_dropout_ratio,
                                                           pre_or_postLayerNorm,
                                                           Context::Instance().GetGemmAlgos(),
                                                           attn_dropout_checkpoint,
                                                           normalize_invertible,
                                                           gelu_checkpoint,
                                                           stochastic_mode);

    s_transformer_layers[layer_id] = layer;

    std::string dtype = (std::is_same<T, __half>::value) ? "half" : "float";

    std::cout << "layer #" << layer_id << " is created with date type [" << dtype << "]."
              << std::endl;

    return 0;
}

template <typename T>
std::vector<torch::Tensor> ds_transformer_forward(int layer_id,
                                                  const torch::Tensor& input,
                                                  const torch::Tensor& input_mask,
                                                  const torch::Tensor& attn_qkvw,
                                                  const torch::Tensor& attn_qkvb,
                                                  const torch::Tensor& attn_ow,
                                                  const torch::Tensor& attn_ob,
                                                  const torch::Tensor& attn_nw,
                                                  const torch::Tensor& attn_nb,
                                                  const torch::Tensor& inter_w,
                                                  const torch::Tensor& inter_b,
                                                  const torch::Tensor& output_w,
                                                  const torch::Tensor& output_b,
                                                  const torch::Tensor& norm_w,
                                                  const torch::Tensor& norm_b,
                                                  bool training_mode,
                                                  bool prelayernorm,
                                                  bool attn_dropout_checkpoint,
                                                  bool normalize_invertible,
                                                  bool gelu_checkpoint,
                                                  bool checkpoint_disabled)
{
    CHECK_INPUT(input);
    CHECK_INPUT(input_mask);
    CHECK_INPUT(attn_qkvw);
    CHECK_INPUT(attn_qkvb);
    CHECK_INPUT(attn_ow);
    CHECK_INPUT(attn_ob);
    CHECK_INPUT(attn_nw);
    CHECK_INPUT(attn_nb);
    CHECK_INPUT(inter_w);
    CHECK_INPUT(inter_b);
    CHECK_INPUT(output_w);
    CHECK_INPUT(output_b);
    CHECK_INPUT(norm_w);
    CHECK_INPUT(norm_b);

    int bsz = input.size(0);

    const T* input_ptr = (const T*)input.data_ptr();
    const T* input_mask_ptr = (const T*)input_mask.data_ptr();
    const T* attn_qkvw_ptr = (const T*)attn_qkvw.data_ptr();
    const T* attn_qkvb_ptr = (const T*)attn_qkvb.data_ptr();
    const T* attn_ow_ptr = (const T*)attn_ow.data_ptr();
    const T* attn_ob_ptr = (const T*)attn_ob.data_ptr();
    const T* attn_nw_ptr = (const T*)attn_nw.data_ptr();
    const T* attn_nb_ptr = (const T*)attn_nb.data_ptr();
    const T* inter_w_ptr = (const T*)inter_w.data_ptr();
    const T* inter_b_ptr = (const T*)inter_b.data_ptr();
    const T* output_w_ptr = (const T*)output_w.data_ptr();
    const T* output_b_ptr = (const T*)output_b.data_ptr();
    const T* norm_w_ptr = (const T*)norm_w.data_ptr();
    const T* norm_b_ptr = (const T*)norm_b.data_ptr();

    auto output = torch::empty_like(input);
    T* out_ptr = (T*)output.data_ptr();

    auto options = torch::TensorOptions()
                       .dtype(input.options().dtype())
                       .layout(torch::kStrided)
                       .device(torch::kCUDA)
                       .requires_grad(true);

    auto uint8_options = torch::TensorOptions()
                             .dtype(torch::kInt8)
                             .layout(torch::kStrided)
                             .device(torch::kCUDA)
                             .requires_grad(false);

    std::shared_ptr<BertTransformerLayer<T>> layer =
        std::static_pointer_cast<BertTransformerLayer<T>>(s_transformer_layers[layer_id]);

    auto inp_norm = ((prelayernorm || !normalize_invertible) ? torch::empty_like(input) : output);
    auto add_res = (normalize_invertible ? inp_norm : torch::empty_like(input));
    auto attn_o_inp = torch::empty_like(input);
    auto qkv_tf = torch::empty({(bsz * layer->GetSeqLength()), output_w.size(0) * 3}, options);

    auto attn_prob_dropout_mask =
        torch::empty({(bsz * layer->GetNumHeads() * layer->GetSeqLength()), layer->GetSeqLength()},
                     uint8_options);
    auto attn_output_dropout_mask =
        torch::empty({(bsz * layer->GetSeqLength()), layer->GetHiddenSize()}, uint8_options);
    auto layer_output_dropout_mask =
        torch::empty({(bsz * layer->GetSeqLength()), layer->GetHiddenSize()}, uint8_options);

    T* inp_norm_ptr = (T*)inp_norm.data_ptr();
    T* add_res_ptr = (T*)add_res.data_ptr();
    T* q_tf_ptr = (T*)qkv_tf.data_ptr();
    T* k_tf_ptr =
        q_tf_ptr + (bsz * layer->GetSeqLength() * output_w.size(0));  //(T*)k_tf.data_ptr();
    T* v_tf_ptr =
        k_tf_ptr + (bsz * layer->GetSeqLength() * output_w.size(0));  //(T*)v_tf.data_ptr();
    T* attn_o_inp_ptr = (T*)attn_o_inp.data_ptr();

    torch::Tensor ff2_inp =
        torch::empty({(bsz * layer->GetSeqLength()), output_w.size(1)}, options);
    torch::Tensor gelu_inp =
        (gelu_checkpoint
             ? ff2_inp
             : torch::empty({(bsz * layer->GetSeqLength()), output_w.size(1)}, options));
    auto ff1_inp = torch::empty_like(input);
    T* ff2_inp_ptr = (T*)ff2_inp.data_ptr();
    T* gelu_inp_ptr = (T*)gelu_inp.data_ptr();
    T* ff1_inp_ptr = (T*)ff1_inp.data_ptr();

    torch::Tensor soft_out = torch::empty(
        {(bsz * layer->GetNumHeads() * layer->GetSeqLength()), layer->GetSeqLength()}, options);
    torch::Tensor ctx_bufB =
        (attn_dropout_checkpoint
             ? soft_out
             : torch::empty(
                   {(bsz * layer->GetNumHeads() * layer->GetSeqLength()), layer->GetSeqLength()},
                   options));
    T* soft_out_ptr = (T*)soft_out.data_ptr();
    T* ctx_bufB_ptr = (T*)ctx_bufB.data_ptr();

    layer->SetTrainingMode(training_mode);
    layer->SetIntermediateBuffers((uint8_t*)attn_prob_dropout_mask.data_ptr(),
                                  (uint8_t*)attn_output_dropout_mask.data_ptr(),
                                  (uint8_t*)layer_output_dropout_mask.data_ptr());

    layer->Forward(bsz,
                   input_ptr,
                   input_mask_ptr,
                   attn_qkvw_ptr,
                   attn_qkvb_ptr,
                   attn_ow_ptr,
                   attn_ob_ptr,
                   attn_nw_ptr,
                   attn_nb_ptr,
                   inter_w_ptr,
                   inter_b_ptr,
                   output_w_ptr,
                   output_b_ptr,
                   norm_w_ptr,
                   norm_b_ptr,
                   out_ptr,
                   inp_norm_ptr,
                   q_tf_ptr,
                   k_tf_ptr,
                   v_tf_ptr,
                   soft_out_ptr,
                   ctx_bufB_ptr,
                   attn_o_inp_ptr,
                   add_res_ptr,
                   ff1_inp_ptr,
                   gelu_inp_ptr,
                   ff2_inp_ptr);

    return {output,
            inp_norm,
            qkv_tf,
            soft_out,
            ctx_bufB,
            attn_o_inp,
            add_res,
            ff1_inp,
            gelu_inp,
            ff2_inp,
            attn_prob_dropout_mask,
            attn_output_dropout_mask,
            layer_output_dropout_mask};
}

template <typename T>
std::vector<torch::Tensor> ds_transformer_backward(int layer_id,
                                                   const torch::Tensor& grad_output,
                                                   const torch::Tensor& output,
                                                   const torch::Tensor& inp_norm,
                                                   const torch::Tensor& qkv_tf,
                                                   const torch::Tensor& soft_out,
                                                   const torch::Tensor& ctx_bufB,
                                                   const torch::Tensor& attn_o_inp,
                                                   const torch::Tensor& add_res,
                                                   const torch::Tensor& ff1_inp,
                                                   const torch::Tensor& gelu_inp,
                                                   const torch::Tensor& ff2_inp,
                                                   const torch::Tensor& attn_prob_dropout_mask,
                                                   const torch::Tensor& attn_output_dropout_mask,
                                                   const torch::Tensor& layer_output_dropout_mask,
                                                   const torch::Tensor& input,
                                                   const torch::Tensor& input_mask,
                                                   const torch::Tensor& attn_qkvw,
                                                   const torch::Tensor& attn_qkvb,
                                                   const torch::Tensor& attn_ow,
                                                   const torch::Tensor& attn_ob,
                                                   const torch::Tensor& attn_nw,
                                                   const torch::Tensor& attn_nb,
                                                   const torch::Tensor& inter_w,
                                                   const torch::Tensor& inter_b,
                                                   const torch::Tensor& output_w,
                                                   const torch::Tensor& output_b,
                                                   const torch::Tensor& norm_w,
                                                   const torch::Tensor& norm_b)
{
    auto g_output = grad_output.contiguous();
    CHECK_INPUT(g_output);
    CHECK_INPUT(output);
    CHECK_INPUT(inp_norm);
    CHECK_INPUT(qkv_tf);
    CHECK_INPUT(add_res);
    CHECK_INPUT(soft_out);
    CHECK_INPUT(ctx_bufB);
    CHECK_INPUT(attn_o_inp);
    CHECK_INPUT(ff1_inp);
    CHECK_INPUT(gelu_inp);
    CHECK_INPUT(ff2_inp);
    CHECK_INPUT(input);
    CHECK_INPUT(input_mask);
    CHECK_INPUT(attn_qkvw);
    CHECK_INPUT(attn_qkvb);
    CHECK_INPUT(attn_ow);
    CHECK_INPUT(attn_ob);
    CHECK_INPUT(attn_nw);
    CHECK_INPUT(attn_nb);
    CHECK_INPUT(inter_w);
    CHECK_INPUT(inter_b);
    CHECK_INPUT(output_w);
    CHECK_INPUT(output_b);
    CHECK_INPUT(norm_w);
    CHECK_INPUT(norm_b);

    int bsz = g_output.size(0);
    std::shared_ptr<BertTransformerLayer<T>> layer =
        std::static_pointer_cast<BertTransformerLayer<T>>(s_transformer_layers[layer_id]);

    auto grad_input = torch::empty_like(input);
    auto grad_attn_qkvw = torch::empty_like(attn_qkvw);
    auto grad_attn_qkvb = torch::empty_like(attn_qkvb);
    auto grad_attn_ow = torch::empty_like(attn_ow);
    auto grad_attn_ob = torch::empty_like(attn_ob);
    auto grad_attn_nw = torch::empty_like(attn_nw);
    auto grad_attn_nb = torch::empty_like(attn_nb);
    auto grad_inter_w = torch::empty_like(inter_w);
    auto grad_inter_b = torch::empty_like(inter_b);
    auto grad_output_w = torch::empty_like(output_w);
    auto grad_output_b = torch::empty_like(output_b);
    auto grad_norm_w = torch::empty_like(norm_w);
    auto grad_norm_b = torch::empty_like(norm_b);

    // inputs.
    const T* grad_output_ptr = (const T*)g_output.data_ptr();
    const T* input_ptr = (const T*)input.data_ptr();
    const T* output_ptr = (const T*)output.data_ptr();
    const T* inp_norm_ptr = (const T*)inp_norm.data_ptr();
    const T* q_tf_ptr = (const T*)qkv_tf.data_ptr();
    const T* add_res_ptr = (const T*)add_res.data_ptr();
    const T* k_tf_ptr =
        q_tf_ptr + (bsz * layer->GetSeqLength() * output_w.size(0));  //(const T*)k_tf.data_ptr();
    const T* v_tf_ptr =
        k_tf_ptr + (bsz * layer->GetSeqLength() * output_w.size(0));  //(const T*)v_tf.data_ptr();
    const T* ff1_inp_ptr = (const T*)ff1_inp.data_ptr();
    const T* gelu_inp_ptr = (const T*)gelu_inp.data_ptr();
    const T* ff2_inp_ptr = (const T*)ff2_inp.data_ptr();
    const T* ctx_bufB_ptr = (const T*)ctx_bufB.data_ptr();
    const T* soft_out_ptr = (const T*)soft_out.data_ptr();
    const T* attn_o_inp_ptr = (const T*)attn_o_inp.data_ptr();
    const T* input_mask_ptr = (const T*)input_mask.data_ptr();
    const T* attn_qkvw_ptr = (const T*)attn_qkvw.data_ptr();
    const T* attn_ow_ptr = (const T*)attn_ow.data_ptr();
    const T* attn_nw_ptr = (const T*)attn_nw.data_ptr();
    const T* attn_nb_ptr = (const T*)attn_nb.data_ptr();
    const T* inter_w_ptr = (const T*)inter_w.data_ptr();
    const T* inter_b_ptr = (const T*)inter_b.data_ptr();
    const T* output_w_ptr = (const T*)output_w.data_ptr();
    const T* norm_w_ptr = (const T*)norm_w.data_ptr();
    const T* norm_b_ptr = (const T*)norm_b.data_ptr();

    // outputs.
    T* grad_input_ptr = (T*)grad_input.data_ptr();
    T* grad_attn_qkvw_ptr = (T*)grad_attn_qkvw.data_ptr();
    T* grad_attn_qkvb_ptr = (T*)grad_attn_qkvb.data_ptr();
    T* grad_attn_ow_ptr = (T*)grad_attn_ow.data_ptr();
    T* grad_attn_ob_ptr = (T*)grad_attn_ob.data_ptr();
    T* grad_attn_nw_ptr = (T*)grad_attn_nw.data_ptr();
    T* grad_attn_nb_ptr = (T*)grad_attn_nb.data_ptr();
    T* grad_inter_w_ptr = (T*)grad_inter_w.data_ptr();
    T* grad_inter_b_ptr = (T*)grad_inter_b.data_ptr();
    T* grad_output_w_ptr = (T*)grad_output_w.data_ptr();
    T* grad_output_b_ptr = (T*)grad_output_b.data_ptr();
    T* grad_norm_w_ptr = (T*)grad_norm_w.data_ptr();
    T* grad_norm_b_ptr = (T*)grad_norm_b.data_ptr();

    layer->SetIntermediateBuffers((uint8_t*)attn_prob_dropout_mask.data_ptr(),
                                  (uint8_t*)attn_output_dropout_mask.data_ptr(),
                                  (uint8_t*)layer_output_dropout_mask.data_ptr());

    layer->Backward(bsz,
                    grad_output_ptr,
                    input_ptr,
                    output_ptr,
                    inp_norm_ptr,
                    q_tf_ptr,
                    k_tf_ptr,
                    v_tf_ptr,
                    soft_out_ptr,
                    ctx_bufB_ptr,
                    attn_o_inp_ptr,
                    add_res_ptr,
                    ff1_inp_ptr,
                    gelu_inp_ptr,
                    ff2_inp_ptr,
                    input_mask_ptr,
                    attn_qkvw_ptr,
                    attn_ow_ptr,
                    attn_nw_ptr,
                    attn_nb_ptr,
                    inter_w_ptr,
                    inter_b_ptr,
                    output_w_ptr,
                    norm_w_ptr,
                    norm_b_ptr,

                    grad_input_ptr,
                    grad_attn_qkvw_ptr,
                    grad_attn_qkvb_ptr,
                    grad_attn_ow_ptr,
                    grad_attn_ob_ptr,
                    grad_attn_nw_ptr,
                    grad_attn_nb_ptr,
                    grad_inter_w_ptr,
                    grad_inter_b_ptr,
                    grad_output_w_ptr,
                    grad_output_b_ptr,
                    grad_norm_w_ptr,
                    grad_norm_b_ptr);

    return {grad_input,
            grad_attn_qkvw,
            grad_attn_qkvb,
            grad_attn_ow,
            grad_attn_ob,
            grad_attn_nw,
            grad_attn_nb,
            grad_inter_w,
            grad_inter_b,
            grad_output_w,
            grad_output_b,
            grad_norm_w,
            grad_norm_b};
}


/***************************************
MLP Layer
Inter = GEMM(input)
Inter = Gelu(Inter+bias)
O = GEMM(Inter)
***************************************/

template <typename T>
BertMlpLayer<T>::BertMlpLayer(int layer_id,
                              int batch_size,
                              int hidden_size,
                              int num_heads,
                              int intermediate_size,
                              int seq_length,
                              const std::vector<std::array<int, 3>>& gemm_algos,
                              bool gelu_checkpoint,
                              bool stochastic_mode)
    : _layer_id(layer_id),
      _batch_size(batch_size),
      _hidden_size(hidden_size),
      _intermediate_size(intermediate_size),
      _heads(num_heads),
      _seq_length(seq_length),
      _training(true),
      _gelu_checkpoint(gelu_checkpoint),
      _stochastic_mode(stochastic_mode),
      _stream(Context::Instance().GetCurrentStream()),
      _cublasHandle(Context::Instance().GetCublasHandle()),
      _ff1(typename FeedForward<T>::Config(batch_size * seq_length,
                                           _intermediate_size,
                                           hidden_size,
                                           gemm_algos[1])),
      _ff2(typename FeedForward<T>::Config(batch_size * seq_length,
                                           hidden_size,
                                           _intermediate_size,
                                           gemm_algos[2])),
      _gelu(typename Gelu<T>::Config(_batch_size, _seq_length, _intermediate_size))
{
    assert(_hidden_size % _heads == 0);
    assert(_seq_length <= 1024);
    Initialize();
}

template <typename T>
BertMlpLayer<T>::~BertMlpLayer()
{
}

template <typename T>
void BertMlpLayer<T>::Initialize()
{
    Context::Instance().GenWorkSpace(get_workspace_size<T>(
        _batch_size, _seq_length, _hidden_size, _heads, _training, _gelu_checkpoint));

    if (std::is_same<T, __half>::value) cublasSetMathMode(_cublasHandle, CUBLAS_TENSOR_OP_MATH);
#ifdef PERF_TEST
    for(int i = 0;i < 3;i++)
        timing_forward[i] = 0;
    for(int i = 0;i < 3;i++)
        timing_backward[i] = 0;
#endif
}

template <typename T>
void BertMlpLayer<T>::Forward(int bsz,
                              const T* input_ptr,
                              const T* inter_w_ptr,
                              const T* inter_b_ptr,
                              const T* output_w_ptr,
                              const T* output_b_ptr,
                              T* out_ptr,
                              T* gelu_inp_ptr,
                              T* ff2_inp_ptr)
{
    cublasSetStream(_cublasHandle, _stream);
    int local_rank = Context::Instance().Get_local_rank();
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);

    T* workspace = static_cast<T*>(Context::Instance().GetWorkSpace());
    size_t small_buf_size = bsz * _seq_length * _hidden_size;
    T* buf_0 = workspace;

    int bsz_seq = bsz * _seq_length;

#ifdef PERF_TEST
    int index = 0;
    float delay;
    GPUTimer t1;
#endif

#ifdef PERF_TEST
    t1.Record();
#endif
    _ff1.Forward(bsz_seq,
                 input_ptr,
                 inter_w_ptr,
                 (_gelu_checkpoint ? ff2_inp_ptr : gelu_inp_ptr),
                 _cublasHandle);
#ifdef PERF_TEST
    if(local_rank == 0)cout << "MLPLayer FWD: \n";
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif

#ifdef PERF_TEST
    t1.Record();
#endif
    _gelu.ForwardWithBiasAdd(bsz,
                             (_gelu_checkpoint ? ff2_inp_ptr : gelu_inp_ptr),
                             inter_b_ptr,
                             (_gelu_checkpoint ? buf_0 : ff2_inp_ptr),
                             _stream);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif

#ifdef PERF_TEST
    t1.Record();
#endif
    _ff2.Forward(bsz_seq,
                 (_gelu_checkpoint ? buf_0 : ff2_inp_ptr),
                 output_w_ptr,
                 out_ptr,
                 _cublasHandle);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
}

template <typename T>
void BertMlpLayer<T>::Backward(int bsz,
                               const T* grad_output_ptr,
                               const T* input_ptr,
                               const T* gelu_inp_ptr,
                               const T* ff2_inp_ptr,
                               const T* inter_w_ptr,
                               const T* inter_b_ptr,
                               const T* output_w_ptr,

                               T* grad_input_ptr,
                               T* grad_inter_w_ptr,
                               T* grad_inter_b_ptr,
                               T* grad_output_w_ptr,
                               T* grad_output_b_ptr)
{
    cublasSetStream(_cublasHandle, _stream);
    int local_rank = Context::Instance().Get_local_rank();
#ifdef PERF_TEST
    int index = 0;
    float delay;
    GPUTimer t1;
#endif
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);

    T* workspace = static_cast<T*>(Context::Instance().GetWorkSpace());
    size_t small_buf_size = bsz * _seq_length * _hidden_size;
    T* buf_0 = workspace;
    T* buf_1 = buf_0 + 4 * small_buf_size;

    int bsz_seq = bsz * _seq_length;
#ifdef PERF_TEST
    t1.Record();
#endif
    if (_gelu_checkpoint) _gelu.ForwardWithBiasAdd(bsz, ff2_inp_ptr, inter_b_ptr, buf_0, _stream);
    _ff2.Backward(bsz_seq,
                  grad_output_ptr,
                  (_gelu_checkpoint ? buf_0 : ff2_inp_ptr),
                  output_w_ptr,
                  grad_output_w_ptr,
                  grad_output_b_ptr,
                  _cublasHandle,
                  _stream,
                  buf_1);
#ifdef PERF_TEST
    if(local_rank == 0)cout << "MLPLayer BWD: \n";
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _gelu.Backward(
        bsz, buf_1, (_gelu_checkpoint ? ff2_inp_ptr : gelu_inp_ptr), inter_b_ptr, _stream);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _ff1.Backward(bsz_seq,
                  buf_1,
                  input_ptr,
                  inter_w_ptr,
                  grad_inter_w_ptr,
                  grad_inter_b_ptr,
                  _cublasHandle,
                  _stream,
                  grad_input_ptr);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
}

template <typename T>
int create_mlp_layer(int layer_id,
                             int batch_size,
                             int hidden_dim,
                             int num_heads,
                             int intermediate_size,
                             int seq_length,
                             int seed,
                             bool test_gemm,
                             bool gelu_checkpoint,
                             bool stochastic_mode,
                             int local_rank)
{
    Context::Instance().SetSeed(seed);
    Context::Instance().TestGemmFP16(
        test_gemm, batch_size, seq_length, num_heads, hidden_dim / num_heads);
    
    Context::Instance().Set_local_rank(local_rank);
    auto layer = std::make_shared<BertMlpLayer<T>>(layer_id,
                                                   batch_size,
                                                   hidden_dim,
                                                   num_heads,
                                                   intermediate_size,
                                                   seq_length,
                                                   Context::Instance().GetGemmAlgos(),
                                                   gelu_checkpoint,
                                                   stochastic_mode);

    s_mlp_layers[layer_id] = layer;

    std::string dtype = (std::is_same<T, __half>::value) ? "half" : "float";

    std::cout << "layer #" << layer_id << " is created with date type [" << dtype << "]."
              << std::endl;

    return 0;
}

template <typename T>
std::vector<torch::Tensor> ds_mlp_forward(int layer_id,
                                                  const torch::Tensor& input,
                                                  const torch::Tensor& inter_w,
                                                  const torch::Tensor& inter_b,
                                                  const torch::Tensor& output_w,
                                                  const torch::Tensor& output_b,
                                                  bool training_mode,
                                                  bool gelu_checkpoint)
{
    CHECK_INPUT(input);
   
    CHECK_INPUT(inter_w);
    CHECK_INPUT(inter_b);
    CHECK_INPUT(output_w);
    CHECK_INPUT(output_b);

    int bsz = input.size(0);

    const T* input_ptr = (const T*)input.data_ptr();
    const T* inter_w_ptr = (const T*)inter_w.data_ptr();
    const T* inter_b_ptr = (const T*)inter_b.data_ptr();
    const T* output_w_ptr = (const T*)output_w.data_ptr();
    const T* output_b_ptr = (const T*)output_b.data_ptr();

    auto output = torch::empty_like(input);
    T* out_ptr = (T*)output.data_ptr();

    auto options = torch::TensorOptions()
                       .dtype(input.options().dtype())
                       .layout(torch::kStrided)
                       .device(torch::kCUDA)
                       .requires_grad(true);

    std::shared_ptr<BertMlpLayer<T>> layer =
        std::static_pointer_cast<BertMlpLayer<T>>(s_mlp_layers[layer_id]);

    torch::Tensor ff2_inp =
        torch::empty({(bsz * layer->GetSeqLength()), output_w.size(1)}, options);
    torch::Tensor gelu_inp =
        (gelu_checkpoint
             ? ff2_inp
             : torch::empty({(bsz * layer->GetSeqLength()), output_w.size(1)}, options));

    T* ff2_inp_ptr = (T*)ff2_inp.data_ptr();
    T* gelu_inp_ptr = (T*)gelu_inp.data_ptr();

    layer->Forward(bsz,
                   input_ptr,
                   inter_w_ptr,
                   inter_b_ptr,
                   output_w_ptr,
                   output_b_ptr,
                   out_ptr,
                   gelu_inp_ptr,
                   ff2_inp_ptr);

    return {output, 
            gelu_inp,
            ff2_inp};
}

template <typename T>
std::vector<torch::Tensor> ds_mlp_backward(int layer_id,
                                                   const torch::Tensor& grad_output,
                                                   const torch::Tensor& gelu_inp,
                                                   const torch::Tensor& ff2_inp,
                                                   const torch::Tensor& input,
                                                   const torch::Tensor& inter_w,
                                                   const torch::Tensor& inter_b,
                                                   const torch::Tensor& output_w,
                                                   const torch::Tensor& output_b)
{
    auto g_output = grad_output.contiguous();
    CHECK_INPUT(g_output);
    CHECK_INPUT(gelu_inp);
    CHECK_INPUT(ff2_inp);
    CHECK_INPUT(input);
    CHECK_INPUT(inter_w);
    CHECK_INPUT(inter_b);
    CHECK_INPUT(output_w);
    CHECK_INPUT(output_b);

    int bsz = g_output.size(0);
    std::shared_ptr<BertMlpLayer<T>> layer =
        std::static_pointer_cast<BertMlpLayer<T>>(s_mlp_layers[layer_id]);

    auto grad_input = torch::empty_like(input);
    auto grad_inter_w = torch::empty_like(inter_w);
    auto grad_inter_b = torch::empty_like(inter_b);
    auto grad_output_w = torch::empty_like(output_w);
    auto grad_output_b = torch::empty_like(output_b);

    // inputs.
    const T* grad_output_ptr = (const T*)g_output.data_ptr();
    const T* input_ptr = (const T*)input.data_ptr();
    
    const T* gelu_inp_ptr = (const T*)gelu_inp.data_ptr();
    const T* ff2_inp_ptr = (const T*)ff2_inp.data_ptr();
    
    const T* inter_w_ptr = (const T*)inter_w.data_ptr();
    const T* inter_b_ptr = (const T*)inter_b.data_ptr();
    const T* output_w_ptr = (const T*)output_w.data_ptr();
    
    // outputs.
    T* grad_input_ptr = (T*)grad_input.data_ptr();
    
    T* grad_inter_w_ptr = (T*)grad_inter_w.data_ptr();
    T* grad_inter_b_ptr = (T*)grad_inter_b.data_ptr();
    T* grad_output_w_ptr = (T*)grad_output_w.data_ptr();
    T* grad_output_b_ptr = (T*)grad_output_b.data_ptr();

    layer->Backward(bsz,
                    grad_output_ptr,
                    input_ptr,
                    gelu_inp_ptr,
                    ff2_inp_ptr,
                    inter_w_ptr,
                    inter_b_ptr,
                    output_w_ptr,

                    grad_input_ptr,
                    grad_inter_w_ptr,
                    grad_inter_b_ptr,
                    grad_output_w_ptr,
                    grad_output_b_ptr);

    return {grad_input,
            grad_inter_w,
            grad_inter_b,
            grad_output_w,
            grad_output_b};
}


/***************************************
Self-Attention Layer
QKV= Query+Key+Value(input)
Q,K,V = Transform(QKV+bias)
P = QK-GEMM
P = Softmax+Dropout(P)
O = PV-GEMM
O = Transform (O)
O = GEMM(O)
***************************************/


template <typename T>
Self_attentionLayer<T>::Self_attentionLayer(int layer_id,
                                              int batch_size,
                                              int hidden_size,
                                              int num_heads,
                                              int selfattention_size,
                                              int seq_length,
                                              float attn_prob_dropout_ratio,
                                              bool pre_or_postLayerNorm,
                                              const std::vector<std::array<int, 3>>& gemm_algos,
                                              bool attn_dropout_checkpoint,
                                              bool normalize_invertible,
                                              bool stochastic_mode)
    : _layer_id(layer_id),
      _batch_size(batch_size),
      _hidden_size(hidden_size),
      _selfattention_size(selfattention_size),
      _heads(num_heads),
      _seq_length(seq_length),
      _training(true),
      _pre_or_postLayerNorm(pre_or_postLayerNorm),
      _attn_dropout_checkpoint(attn_dropout_checkpoint),
      _normalize_invertible(normalize_invertible),
      _stochastic_mode(stochastic_mode),
      _stream(Context::Instance().GetCurrentStream()),
      _cublasHandle(Context::Instance().GetCublasHandle()),
      _qkv_linear(typename FeedForward<T>::Config(batch_size * seq_length,
                                                  3 * selfattention_size,
                                                  hidden_size,
                                                  gemm_algos[0])),
      _attn_out_linear(typename FeedForward<T>::Config(batch_size * seq_length,
                                                       hidden_size,
                                                       selfattention_size,
                                                       gemm_algos[0])),
      _softmax(typename Softmax<T>::Config(batch_size, num_heads, seq_length)),
      _attn_prob_dropout(typename Dropout<T>::Config(attn_prob_dropout_ratio,
                                                     _batch_size * _heads * _seq_length,
                                                     _seq_length)),
      _attn_scores(typename StridedBatchGemm<T>::Config(_batch_size * _heads,
                                                        _seq_length,
                                                        _seq_length,
                                                        selfattention_size / _heads,
                                                        (T(1.0) / T(sqrt(selfattention_size / _heads))),
                                                        T(0.0),
                                                        CUBLAS_OP_T,
                                                        CUBLAS_OP_N,
                                                        gemm_algos[3])),
      _attn_context(typename StridedBatchGemm<T>::Config(_batch_size * _heads,
                                                         selfattention_size / _heads,
                                                         _seq_length,
                                                         _seq_length,
                                                         T(1.0),
                                                         T(0.0),
                                                         CUBLAS_OP_N,
                                                         CUBLAS_OP_N,
                                                         gemm_algos[4]))
{
    assert(_hidden_size % _heads == 0);
    assert(_seq_length <= 1024);
    Initialize();
}

template <typename T>
Self_attentionLayer<T>::~Self_attentionLayer()
{
    //cout << "SOFTMAX BACKWARD: " << soft_backward << endl;
}

template <typename T>
void Self_attentionLayer<T>::Initialize()
{
    Context::Instance().GenWorkSpace(get_workspace_size<T>(_batch_size, _seq_length, _hidden_size, _heads, _training));

    if (std::is_same<T, __half>::value) cublasSetMathMode(_cublasHandle, CUBLAS_TENSOR_OP_MATH);
#ifdef PERF_TEST
    for(int i = 0;i < 8;i++)
        timing_forward[i] = 0;

    for(int i = 0;i < 8;i++)
        timing_backward[i] = 0;
#endif
}

template <typename T>
void Self_attentionLayer<T>::Forward(int bsz,
                                      const T* input_ptr,
                                      const T* input_mask_ptr,
                                      const T* attn_qkvw_ptr,
                                      const T* attn_qkvb_ptr,
                                      const T* attn_ow_ptr,
                                      const T* attn_ob_ptr,
                                      T* out_ptr,
                                      T* q_tf_ptr,
                                      T* k_tf_ptr,
                                      T* v_tf_ptr,
                                      T* soft_out_ptr,
                                      T* ctx_bufB_ptr,
                                      T* attn_o_inp_ptr)
{
    cublasSetStream(_cublasHandle, _stream);
#ifdef PERF_TEST
    int index = 0;
    float delay = 0;
    GPUTimer t1;
#endif
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);
    int local_rank = Context::Instance().Get_local_rank();

    T* workspace = static_cast<T*>(Context::Instance().GetWorkSpace());
    size_t small_buf_size = bsz * _seq_length * _hidden_size;
    T* buf_0 = workspace;
    T* buf_1 = buf_0 + small_buf_size;

    if (_attn_dropout_checkpoint) ctx_bufB_ptr = buf_1 + 2 * small_buf_size;


    int bsz_seq = bsz * _seq_length;
#ifdef PERF_TEST
    t1.Record();
#endif
    _qkv_linear.Forward(bsz_seq, input_ptr, attn_qkvw_ptr, buf_0, _cublasHandle);
#ifdef PERF_TEST
    if(local_rank == 0)cout << "Self_attentionLayer FWD: \n";
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    launch_bias_add_transform_0213<T>(
        q_tf_ptr, buf_0, attn_qkvb_ptr, bsz, _seq_length, _selfattention_size, _heads, _stream, 3);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
    int bsz_heads = bsz * _heads;
#ifdef PERF_TEST
    t1.Record();
#endif
    // attention scores
    _attn_scores.Forward(bsz_heads, soft_out_ptr, k_tf_ptr, q_tf_ptr, _cublasHandle);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    // Softmax + Mask
    _softmax.Forward1(bsz, soft_out_ptr, nullptr, _stream);
    //_softmax.Forward_fused_dropout(bsz, soft_out_ptr, input_mask_ptr, 
      //          _attn_prob_dropout.GetMask(), _attn_prob_dropout.GetRatio(), _stream, _threads);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    // attn prob dropout.
    _attn_prob_dropout.Forward(bsz_heads * _seq_length, ctx_bufB_ptr, soft_out_ptr, _stream);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    // attention context
    _attn_context.Forward(bsz_heads, buf_1, v_tf_ptr, ctx_bufB_ptr, _cublasHandle);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    launch_transform4d_0213<T>(
        attn_o_inp_ptr, buf_1, bsz, _heads, _seq_length, _selfattention_size, _stream, 1);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _attn_out_linear.Forward(bsz_seq, attn_o_inp_ptr, attn_ow_ptr, out_ptr, _cublasHandle);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif 
}

template <typename T>
void Self_attentionLayer<T>::Backward(int bsz,
                                       const T* grad_output_ptr,
                                       const T* input_ptr,
                                       const T* output_ptr,
                                       const T* q_tf_ptr,
                                       const T* k_tf_ptr,
                                       const T* v_tf_ptr,
                                       const T* soft_out_ptr,
                                       const T* ctx_bufB_ptr,
                                       const T* attn_o_inp_ptr,
                                       const T* input_mask_ptr,
                                       const T* attn_qkvw_ptr,
                                       const T* attn_ow_ptr,

                                       T* grad_input_ptr,
                                       T* grad_attn_qkvw_ptr,
                                       T* grad_attn_qkvb_ptr,
                                       T* grad_attn_ow_ptr,
                                       T* grad_attn_ob_ptr)
{
    cublasSetStream(_cublasHandle, _stream);

    if (!_stochastic_mode) cudaStreamSynchronize(_stream);
    int local_rank = Context::Instance().Get_local_rank();
    T* workspace = static_cast<T*>(Context::Instance().GetWorkSpace());
    size_t small_buf_size = bsz * _seq_length * _selfattention_size;
    T* buf_0 = workspace;
    T* buf_1 = buf_0 + small_buf_size;
    T* buf_2 = buf_1 + small_buf_size;
    T* buf_3 = buf_2 + small_buf_size;

    T* ff2_buf = buf_3 + small_buf_size;
    T* ctx_bufB_ptr_recomp = buf_3 + (_seq_length * _seq_length * bsz * _heads);
#ifdef PERF_TEST
    int index = 0;
    float delay;
    GPUTimer t1;
#endif
    cudaStream_t streams[2] = {_stream, _stream};

    int bsz_seq = bsz * _seq_length;
    int bsz_heads = bsz * _heads;

#ifdef PERF_TEST
    t1.Record();
#endif
    _attn_out_linear.Backward(bsz_seq,
                              grad_output_ptr,
                              attn_o_inp_ptr,
                              attn_ow_ptr,
                              grad_attn_ow_ptr,
                              grad_attn_ob_ptr,
                              _cublasHandle,
                              _stream,
                              buf_1);
#ifdef PERF_TEST
    //if(local_rank == 0)cout << "Self_attentionLayer BWD: \n";
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    launch_transform_0213<T>(buf_2, buf_1, bsz, _seq_length, _selfattention_size, _heads, _stream);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    if (_attn_prob_dropout.HasDropout()) {
        if (_attn_dropout_checkpoint)
            _attn_prob_dropout.Forward(
                bsz_heads * _seq_length, ctx_bufB_ptr_recomp, soft_out_ptr, _stream, true);

        _attn_context.Backward(bsz_heads,
                               buf_2,
                               v_tf_ptr,
                               (_attn_dropout_checkpoint ? ctx_bufB_ptr_recomp : ctx_bufB_ptr),
                               _cublasHandle,
                               buf_3,
                               ff2_buf);
    } else
        _attn_context.Backward(bsz_heads, buf_2, v_tf_ptr, 
                                soft_out_ptr, _cublasHandle, buf_3, ff2_buf);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _attn_prob_dropout.Backward(bsz_heads * _seq_length, ff2_buf, _stream);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _softmax.Backward(bsz, ff2_buf, soft_out_ptr, _stream);
    //_softmax.Backward_fused_dropout(bsz, ff2_buf, soft_out_ptr, 
      //      _attn_prob_dropout.GetMask(), _attn_prob_dropout.GetRatio(), _stream, _threads);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    soft_backward += delay;
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _attn_scores.Backward(bsz_heads, ff2_buf, k_tf_ptr, q_tf_ptr, _cublasHandle, buf_2, buf_1);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    launch_transform4d_0213(ff2_buf, buf_1, bsz, _heads, _seq_length, _selfattention_size, _stream, 3);
#ifdef PERF_TEST
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _qkv_linear.Backward(bsz_seq,
                             ff2_buf,
                             input_ptr,
                             attn_qkvw_ptr,
                             grad_attn_qkvw_ptr,
                             grad_attn_qkvb_ptr,
                             _cublasHandle,
                             _stream,
                             grad_input_ptr);
                    
#ifdef PERF_TEST
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
}

template <typename T>
void Self_attentionLayer<T>::SetTrainingMode(bool training)
{
    // Dropout will be skipped when not in training model.
    _attn_prob_dropout.SetTrainingMode(training);
}

template <typename T>
void Self_attentionLayer<T>::SetIntermediateBuffers(uint8_t* attn_prob_dropout_mask_ptr)
{
    _attn_prob_dropout.SetMask(attn_prob_dropout_mask_ptr);
}


template <typename T>
int create_self_attention_layer(int layer_id,
                             int batch_size,
                             int hidden_dim,
                             int selfattention_size,
                             int num_heads,
                             int seq_length,
                             float attn_dropout_ratio,
                             int seed,
                             bool pre_or_postLayerNorm,
                             bool test_gemm,
                             bool attn_dropout_checkpoint,
                             bool normalize_invertible,
                             bool stochastic_mode,
                             int local_rank)
{
    Context::Instance().SetSeed(seed);
    Context::Instance().TestGemmFP16(
        test_gemm, batch_size, seq_length, num_heads, hidden_dim / num_heads);
    
    Context::Instance().Set_local_rank(local_rank);
    auto layer = std::make_shared<Self_attentionLayer<T>>(layer_id,
                                                           batch_size,
                                                           hidden_dim,
                                                           num_heads,
                                                           selfattention_size,
                                                           seq_length,
                                                           attn_dropout_ratio,
                                                           pre_or_postLayerNorm,
                                                           Context::Instance().GetGemmAlgos(),
                                                           attn_dropout_checkpoint,
                                                           normalize_invertible,
                                                           stochastic_mode);

    s_self_attention_layers[layer_id] = layer;

    std::string dtype = (std::is_same<T, __half>::value) ? "half" : "float";

    std::cout << "layer #" << layer_id << " is created with date type [" << dtype << "]."
              << std::endl;

    return 0;
}

template <typename T>
std::vector<torch::Tensor> ds_self_attention_forward(int layer_id,
                                                  const torch::Tensor& input,
                                                  const torch::Tensor& input_mask,
                                                  const torch::Tensor& attn_qkvw,
                                                  const torch::Tensor& attn_qkvb,
                                                  const torch::Tensor& attn_ow,
                                                  const torch::Tensor& attn_ob,
                                                  bool training_mode,
                                                  bool prelayernorm,
                                                  bool attn_dropout_checkpoint,
                                                  bool grad_enable)
{
    CHECK_INPUT(input);
    CHECK_INPUT(input_mask);
    CHECK_INPUT(attn_qkvw);
    CHECK_INPUT(attn_qkvb);
    CHECK_INPUT(attn_ow);
    CHECK_INPUT(attn_ob);

    int bsz = input.size(0);

    Context::Instance().Enable_Grad(grad_enable);

    const T* input_ptr = (const T*)input.data_ptr();
    const T* input_mask_ptr = (const T*)input_mask.data_ptr();
    const T* attn_qkvw_ptr = (const T*)attn_qkvw.data_ptr();
    const T* attn_qkvb_ptr = (const T*)attn_qkvb.data_ptr();
    const T* attn_ow_ptr = (const T*)attn_ow.data_ptr();
    const T* attn_ob_ptr = (const T*)attn_ob.data_ptr();

    auto output = torch::empty_like(input);
    T* out_ptr = (T*)output.data_ptr();

    auto options = torch::TensorOptions()
                       .dtype(input.options().dtype())
                       .layout(torch::kStrided)
                       .device(torch::kCUDA)
                       .requires_grad(true);

    auto uint8_options = torch::TensorOptions()
                             .dtype(torch::kInt8)
                             .layout(torch::kStrided)
                             .device(torch::kCUDA)
                             .requires_grad(false);

    std::shared_ptr<Self_attentionLayer<T>> layer =
        std::static_pointer_cast<Self_attentionLayer<T>>(s_self_attention_layers[layer_id]);

    auto attn_o_inp = torch::empty_like(input);
    auto qkv_tf = torch::empty({(bsz * 3), layer->GetNumHeads(), layer->GetSeqLength(), (layer->GetSelfAttentionSize() / layer->GetNumHeads())}, options);

    //auto attn_prob_dropout_mask =
    //    torch::empty({(bsz * layer->GetNumHeads() * layer->GetSeqLength()), layer->GetSeqLength()},
    //                 uint8_options);

    T* q_tf_ptr = (T*)qkv_tf.data_ptr();
    T* k_tf_ptr =
        q_tf_ptr + (bsz * layer->GetSeqLength() * layer->GetSelfAttentionSize());  //(T*)k_tf.data_ptr();
    T* v_tf_ptr =
        k_tf_ptr + (bsz * layer->GetSeqLength() * layer->GetSelfAttentionSize());  //(T*)v_tf.data_ptr();
    T* attn_o_inp_ptr = (T*)attn_o_inp.data_ptr();

    torch::Tensor soft_out = torch::empty(
        {(bsz * layer->GetNumHeads() * layer->GetSeqLength()), layer->GetSeqLength()}, options);
    torch::Tensor ctx_bufB =
        (attn_dropout_checkpoint
             ? soft_out
             : torch::empty(
                   {(bsz * layer->GetNumHeads() * layer->GetSeqLength()), layer->GetSeqLength()},
                   options));
    T* soft_out_ptr = (T*)soft_out.data_ptr();
    T* ctx_bufB_ptr = (T*)ctx_bufB.data_ptr();

    layer->SetTrainingMode(training_mode);
    //layer->SetIntermediateBuffers((uint8_t*)attn_prob_dropout_mask.data_ptr());

    layer->Forward(bsz,
                   input_ptr,
                   input_mask_ptr,
                   attn_qkvw_ptr,
                   attn_qkvb_ptr,
                   attn_ow_ptr,
                   attn_ob_ptr,
                   out_ptr,
                   q_tf_ptr,
                   k_tf_ptr,
                   v_tf_ptr,
                   soft_out_ptr,
                   ctx_bufB_ptr,
                   attn_o_inp_ptr);

    return {output,
            qkv_tf,
            soft_out,
            ctx_bufB,
            attn_o_inp};
            //attn_prob_dropout_mask};
}

template <typename T>
std::vector<torch::Tensor> ds_self_attention_backward(int layer_id,
                                                   const torch::Tensor& grad_output,
                                                   const torch::Tensor& output,
                                                   const torch::Tensor& qkv_tf,
                                                   const torch::Tensor& soft_out,
                                                   const torch::Tensor& ctx_bufB,
                                                   const torch::Tensor& attn_o_inp,
                                                   //const torch::Tensor& attn_prob_dropout_mask,
                                                   const torch::Tensor& input,
                                                   const torch::Tensor& input_mask,
                                                   const torch::Tensor& attn_qkvw,
                                                   const torch::Tensor& attn_qkvb,
                                                   const torch::Tensor& attn_ow,
                                                   const torch::Tensor& attn_ob)
{
    auto g_output = grad_output.contiguous();
    CHECK_INPUT(g_output);
    CHECK_INPUT(output);
    CHECK_INPUT(qkv_tf);
    CHECK_INPUT(soft_out);
    CHECK_INPUT(ctx_bufB);
    CHECK_INPUT(attn_o_inp);
    CHECK_INPUT(input);
    CHECK_INPUT(input_mask);
    CHECK_INPUT(attn_qkvw);
    CHECK_INPUT(attn_qkvb);
    CHECK_INPUT(attn_ow);
    CHECK_INPUT(attn_ob);

    int bsz = g_output.size(0);
    std::shared_ptr<Self_attentionLayer<T>> layer =
        std::static_pointer_cast<Self_attentionLayer<T>>(s_self_attention_layers[layer_id]);

    auto grad_input = torch::empty_like(input);
    auto grad_attn_qkvw = torch::empty_like(attn_qkvw);
    auto grad_attn_qkvb = torch::empty_like(attn_qkvb);
    auto grad_attn_ow = torch::empty_like(attn_ow);
    auto grad_attn_ob = torch::empty_like(attn_ob);

    // inputs.
    const T* grad_output_ptr = (const T*)g_output.data_ptr();
    const T* input_ptr = (const T*)input.data_ptr();
    const T* output_ptr = (const T*)output.data_ptr();
    const T* q_tf_ptr = (const T*)qkv_tf.data_ptr();
    const T* k_tf_ptr =
        q_tf_ptr + (bsz * layer->GetSeqLength() * layer->GetSelfAttentionSize());  //(const T*)k_tf.data_ptr();
    const T* v_tf_ptr =
        k_tf_ptr + (bsz * layer->GetSeqLength() * layer->GetSelfAttentionSize());  //(const T*)v_tf.data_ptr();
    const T* ctx_bufB_ptr = (const T*)ctx_bufB.data_ptr();
    const T* soft_out_ptr = (const T*)soft_out.data_ptr();
    const T* attn_o_inp_ptr = (const T*)attn_o_inp.data_ptr();
    const T* input_mask_ptr = (const T*)input_mask.data_ptr();
    const T* attn_qkvw_ptr = (const T*)attn_qkvw.data_ptr();
    const T* attn_ow_ptr = (const T*)attn_ow.data_ptr();

    // outputs.
    T* grad_input_ptr = (T*)grad_input.data_ptr();
    T* grad_attn_qkvw_ptr = (T*)grad_attn_qkvw.data_ptr();
    T* grad_attn_qkvb_ptr = (T*)grad_attn_qkvb.data_ptr();
    T* grad_attn_ow_ptr = (T*)grad_attn_ow.data_ptr();
    T* grad_attn_ob_ptr = (T*)grad_attn_ob.data_ptr();

    //layer->SetIntermediateBuffers((uint8_t*)attn_prob_dropout_mask.data_ptr());

    layer->Backward(bsz,
                    grad_output_ptr,
                    input_ptr,
                    output_ptr,
                    q_tf_ptr,
                    k_tf_ptr,
                    v_tf_ptr,
                    soft_out_ptr,
                    ctx_bufB_ptr,
                    attn_o_inp_ptr,
                    input_mask_ptr,
                    attn_qkvw_ptr,
                    attn_ow_ptr,

                    grad_input_ptr,
                    grad_attn_qkvw_ptr,
                    grad_attn_qkvb_ptr,
                    grad_attn_ow_ptr,
                    grad_attn_ob_ptr);

    return {grad_input,
            grad_attn_qkvw,
            grad_attn_qkvb,
            grad_attn_ow,
            grad_attn_ob};
}

/***************************************
Bias Residual and dropout fusion
Y = Dropout (X + bias) + residual
***************************************/

template<typename T>
BiasResidualDropout<T>::BiasResidualDropout(int layer_id,
                                   int batch_size,
                                   int hidden_size,
                                   int num_heads,
                                   int seq_length,
                                   float dropout_ratio,
                                   bool stochastic_mode) : _layer_id(layer_id),
      _batch_size(batch_size),
      _hidden_size(hidden_size),
      _seq_length(seq_length),
      _training(true),
      _stochastic_mode(stochastic_mode),
      _stream(Context::Instance().GetCurrentStream()),
      _dropout(typename Dropout<T>::Config(dropout_ratio,
                                            _batch_size * _seq_length,
                                            _hidden_size))
{
    assert(_hidden_size % _heads == 0);
    assert(_seq_length <= 1024);
#ifdef PERF_TEST
    for(int i = 0;i < 1;i++)
        timing_forward[i] = 0;
    for(int i = 0;i < 1;i++)
        timing_backward[i] = 0;
#endif
}

template<typename T>
BiasResidualDropout<T>::~BiasResidualDropout()
{
#ifdef PERF_TEST
    std::cout << "DROPOUT forward elapsed:\n";
    for(int i = 0;i < 1;i++)std::cout << timing_forward[i]/100 << std::endl;
    std::cout << "DROPOUT backward elapsed:\n";
    for(int i = 0;i < 1;i++)std::cout << timing_backward[i]/100 << std::endl;
#endif
}

template<typename T>
void BiasResidualDropout<T>::Forward(int bsz,
                 const T* input_ptr,
                 const T* residual_ptr,
                 const T* b_ptr,
                 T* out_ptr)
{
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);
    int local_rank = Context::Instance().Get_local_rank();
#ifdef PERF_TEST
    int index = 0;
    float delay;
    GPUTimer t1;
#endif    
#ifdef PERF_TEST
    t1.Record();
#endif
    _dropout.ForwardWithBias(
            (bsz * _seq_length), out_ptr, input_ptr, residual_ptr, b_ptr, _stream);
#ifdef PERF_TEST
    if(local_rank == 0)cout << "BiasResidualDropout FWD: \n";
    t1.Elapsed(delay);
    if(local_rank == 0)cout << delay << endl;
    timing_forward[index++] += delay;
#endif
}

template<typename T>
void BiasResidualDropout<T>::Backward(int bsz,
                  const T* grad_output_ptr,
                  T* grad_input_ptr)
{
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);
    int local_rank = Context::Instance().Get_local_rank();
#ifdef PERF_TEST
    int index = 0;
    float delay;
    GPUTimer t1;
#endif
#ifdef PERF_TEST
    t1.Record();
#endif
    _dropout.Backward((bsz * _seq_length), grad_input_ptr, grad_output_ptr, _stream);
#ifdef PERF_TEST
    //if(local_rank == 0)cout << "BiasResidualDropout BWD: \n";
    t1.Elapsed(delay);
    //if(local_rank == 0)cout << delay << endl;
    timing_backward[index++] += delay;
#endif
}

template <typename T>
void BiasResidualDropout<T>::SetIntermediateBuffers(uint8_t* dropout_mask_ptr)
{
    _dropout.SetMask(dropout_mask_ptr);
}

template <typename T>
void BiasResidualDropout<T>::SetTrainingMode(bool training)
{
    // Dropout will be skipped when not in training model.
    _dropout.SetTrainingMode(training);
}


template <typename T>
int create_bias_residual_dropout_layer(int layer_id,
                             int batch_size,
                             int hidden_dim,
                             int num_heads,
                             int seq_length,
                             float dropout_ratio,
                             int seed,
                             bool test_gemm,
                             bool stochastic_mode,
                             int local_rank)
{
    Context::Instance().SetSeed(seed);
    Context::Instance().TestGemmFP16(
        test_gemm, batch_size, seq_length, num_heads, hidden_dim / num_heads);
    
    Context::Instance().Set_local_rank(local_rank);
    auto layer = std::make_shared<BiasResidualDropout<T>>(layer_id,
                                                   batch_size,
                                                   hidden_dim,
                                                   num_heads,
                                                   seq_length,
                                                   dropout_ratio,
                                                   stochastic_mode);

    s_bias_residual_dropout_layers[layer_id] = layer;

    std::string dtype = (std::is_same<T, __half>::value) ? "half" : "float";

    std::cout << "layer #" << layer_id << " is created with date type [" << dtype << "]."
              << std::endl;

    return 0;
}

template <typename T>
std::vector<torch::Tensor> ds_bias_residual_dropout_forward(int layer_id,
                                          const torch::Tensor& input,
                                          const torch::Tensor& residual,
                                          const torch::Tensor& bias,
                                          bool training_mode,
                                          bool grad_enable)
{
    CHECK_INPUT(input);
    CHECK_INPUT(residual);
    CHECK_INPUT(bias);
   
    int bsz = input.size(0);

    Context::Instance().Enable_Grad(grad_enable);

    const T* input_ptr = (const T*)input.data_ptr();
    const T* residual_ptr = (const T*)residual.data_ptr();
    const T* bias_ptr = (const T*)bias.data_ptr();

    auto output = torch::empty_like(input);
    T* out_ptr = (T*)output.data_ptr();

    auto options = torch::TensorOptions()
                       .dtype(input.options().dtype())
                       .layout(torch::kStrided)
                       .device(torch::kCUDA)
                       .requires_grad(true);

    auto uint8_options = torch::TensorOptions()
                             .dtype(torch::kInt8)
                             .layout(torch::kStrided)
                             .device(torch::kCUDA)
                             .requires_grad(false);

    std::shared_ptr<BiasResidualDropout<T>> layer =
        std::static_pointer_cast<BiasResidualDropout<T>>(s_bias_residual_dropout_layers[layer_id]);
    
    //auto dropout_mask =
    //    torch::empty({(bsz * layer->GetSeqLength()), layer->GetHiddenSize()}, uint8_options);
        
    //cout << "***************\t" << bsz << ' ' << layer->GetSeqLength() << ' ' << layer->GetHiddenSize() << ' ' << (int*)dropout_mask.data_ptr() << endl;
    
    layer->SetTrainingMode(training_mode);
    //layer->SetIntermediateBuffers((uint8_t*)dropout_mask.data_ptr());


    layer->Forward(bsz,
                   input_ptr,
                   residual_ptr,
                   bias_ptr,
                   out_ptr);

    return {output}; //, dropout_mask};
}


template <typename T>
std::vector<torch::Tensor> ds_bias_residual_dropout_backward(int layer_id,
                                           const torch::Tensor& grad_output)
                                           //const torch::Tensor& dropout_mask)
{
    auto g_output = grad_output.contiguous();
    CHECK_INPUT(g_output);

    int bsz = g_output.size(0);

    std::shared_ptr<BiasResidualDropout<T>> layer =
        std::static_pointer_cast<BiasResidualDropout<T>>(s_bias_residual_dropout_layers[layer_id]);

    auto options = torch::TensorOptions()
                       .dtype(grad_output.options().dtype())
                       .layout(torch::kStrided)
                       .device(torch::kCUDA)
                       .requires_grad(true);

    auto grad_input = torch::empty_like(grad_output);

    // inputs.
    const T* grad_output_ptr = (const T*)g_output.data_ptr();

    //layer->SetIntermediateBuffers((uint8_t*)dropout_mask.data_ptr());

    // outputs.
    T* grad_input_ptr = (T*)grad_input.data_ptr();

    layer->Backward(bsz,
                    grad_output_ptr,
                    grad_input_ptr);

    return {grad_input};
}




template<typename T>
LayerNormalize<T>::LayerNormalize(int layer_id,
                                   int batch_size,
                                   int hidden_size,
                                   int num_heads,
                                   int seq_length,
                                   bool normalize_invertible,
                                   bool stochastic_mode) : _layer_id(layer_id),
      _batch_size(batch_size),
      _hidden_size(hidden_size),
      _seq_length(seq_length),
      _training(true),
      _stochastic_mode(stochastic_mode),
      _stream(Context::Instance().GetCurrentStream()),
      _norm_layer(typename Normalize_Layer<T>::Config(batch_size,
                                                      seq_length,
                                                      hidden_size,
                                                      true,
                                                      false,
                                                      false,
                                                      !normalize_invertible))
{
    assert(_hidden_size % _heads == 0);
    assert(_seq_length <= 1024);
}

template<typename T>
LayerNormalize<T>::~LayerNormalize()
{
}

template<typename T>
void LayerNormalize<T>::Forward(int bsz,
                                const T* input_ptr,
                                const T* gamma_ptr,
                                const T* betta_ptr,
                                T* out_ptr)
{
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);
    
    if (_norm_layer.UseMean())
        _norm_layer.ForwardCheckpoint(
            bsz, out_ptr, input_ptr, gamma_ptr, betta_ptr, _stream, true);
    else
        _norm_layer.Forward(
            bsz, out_ptr, input_ptr, gamma_ptr, betta_ptr, _stream, true);
}

template<typename T>
void LayerNormalize<T>::Backward(int bsz,
                                 const T* grad_output_ptr,
                                 const T* inout,
                                 const T* gamma_ptr, 
                                 const T* betta_ptr,
                                 T* grad_input_ptr,
                                 T* grad_gamma_ptr,
                                 T* grad_betta_ptr)
{
    if (!_stochastic_mode) cudaStreamSynchronize(_stream);
    cudaStream_t streams[2] = {_stream, _stream};

    if (_norm_layer.UseMean())
            _norm_layer.Backward(bsz,
                                  grad_output_ptr,
                                  gamma_ptr,
                                  grad_gamma_ptr,
                                  grad_betta_ptr,
                                  streams,
                                  grad_input_ptr,
                                  inout);

        else
            _norm_layer.Backward(bsz,
                                 grad_output_ptr,
                                 gamma_ptr,
                                 betta_ptr,
                                 grad_gamma_ptr,
                                 grad_betta_ptr,
                                 streams,
                                 grad_input_ptr,
                                 inout);
}

template <typename T>
int create_LayerNormalize_layer(int layer_id,
                                int batch_size,
                                int hidden_dim,
                                int num_heads,
                                int seq_length,
                                bool normalize_invertible,
                                int seed,
                                bool test_gemm,
                                bool stochastic_mode)
{
    Context::Instance().SetSeed(seed);
    Context::Instance().TestGemmFP16(
        test_gemm, batch_size, seq_length, num_heads, hidden_dim / num_heads);

    auto layer = std::make_shared<LayerNormalize<T>>(layer_id,
                                                   batch_size,
                                                   hidden_dim,
                                                   num_heads,
                                                   seq_length,
                                                   normalize_invertible,
                                                   stochastic_mode);

    s_LayerNormalize_layers[layer_id] = layer;

    std::string dtype = (std::is_same<T, __half>::value) ? "half" : "float";

    std::cout << "layer #" << layer_id << " is created with date type [" << dtype << "]."
              << std::endl;

    return 0;
}

template <typename T>
std::vector<torch::Tensor> ds_LayerNormalize_forward(int layer_id,
                                                     const torch::Tensor& input,
                                                     const torch::Tensor& gamma,
                                                     const torch::Tensor& betta)
{
    CHECK_INPUT(input);
    CHECK_INPUT(gamma);
    CHECK_INPUT(betta);
   
    int bsz = input.size(0);

    const T* input_ptr = (const T*)input.data_ptr();
    const T* gamma_ptr = (const T*)gamma.data_ptr();
    const T* betta_ptr = (const T*)betta.data_ptr();

    std::shared_ptr<LayerNormalize<T>> layer =
        std::static_pointer_cast<LayerNormalize<T>>(s_LayerNormalize_layers[layer_id]);

    auto output = torch::empty_like(input);
    T* out_ptr = (T*)output.data_ptr();

    layer->Forward(bsz,
                   input_ptr,
                   gamma_ptr,
                   betta_ptr,
                   out_ptr);

    return {output};
}


template <typename T>
std::vector<torch::Tensor> ds_LayerNormalize_backward(int layer_id,
                                                      const torch::Tensor& grad_output,
                                                      const torch::Tensor& inout,
                                                      const torch::Tensor& gamma,
                                                      const torch::Tensor& betta)
{
    auto g_output = grad_output.contiguous();
    CHECK_INPUT(g_output);
    CHECK_INPUT(gamma);
    CHECK_INPUT(betta);
    CHECK_INPUT(inout);

    int bsz = g_output.size(0);

    std::shared_ptr<LayerNormalize<T>> layer =
        std::static_pointer_cast<LayerNormalize<T>>(s_LayerNormalize_layers[layer_id]);

    auto grad_input = torch::empty_like(grad_output);
    auto grad_gamma = torch::empty_like(gamma);
    auto grad_betta = torch::empty_like(betta);

    // inputs.
    const T* grad_output_ptr = (const T*)g_output.data_ptr();
    const T* gamma_ptr = (const T*)gamma.data_ptr();
    const T* betta_ptr = (const T*)betta.data_ptr();
    const T* inout_ptr = (const T*)inout.data_ptr();

    // outputs.
    T* grad_input_ptr = (T*)grad_input.data_ptr();
    T* grad_gamma_ptr = (T*)grad_gamma.data_ptr();
    T* grad_betta_ptr = (T*)grad_betta.data_ptr();

    layer->Backward(bsz,
                    grad_output_ptr,
                    inout_ptr,
                    gamma_ptr,
                    betta_ptr,
                    grad_input_ptr,
                    grad_gamma_ptr,
                    grad_betta_ptr);

    return {grad_input,
            grad_gamma,
            grad_betta};
}

void store_rand_state() { Context::Instance().StoreRandOffset(); }

void restore_rand_state(bool grad_enable) { Context::Instance().RestoreRandOffset(grad_enable); }

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("forward_transformer_fp32",
          &ds_transformer_forward<float>,
          "DeepSpeed Transformer forward with fp32 (CUDA)");
    m.def("forward_transformer_fp16",
          &ds_transformer_forward<__half>,
          "DeepSpeed Transformer forward with fp16 (CUDA)");
    m.def("backward_transformer_fp32",
          &ds_transformer_backward<float>,
          "DeepSpeed Transformer backward with fp32 (CUDA)");
    m.def("backward_transformer_fp16",
          &ds_transformer_backward<__half>,
          "DeepSpeed Transformer backward with fp16 (CUDA)");
    m.def("create_transformer_layer_fp32",
          &create_transformer_layer<float>,
          "Create DeepSpeed Transformer Transformer Layer with fp32 (CUDA)");
    m.def("create_transformer_layer_fp16",
          &create_transformer_layer<__half>,
          "Create DeepSpeed Transformer Transformer Layer with fp16 (CUDA)");
        m.def("forward_self_attention_fp32",
          &ds_self_attention_forward<float>,
          "DeepSpeed self_attention forward with fp32 (CUDA)");
    m.def("forward_self_attention_fp16",
          &ds_self_attention_forward<__half>,
          "DeepSpeed self_attention forward with fp16 (CUDA)");
    m.def("backward_self_attention_fp32",
          &ds_self_attention_backward<float>,
          "DeepSpeed self_attention backward with fp32 (CUDA)");
    m.def("backward_self_attention_fp16",
          &ds_self_attention_backward<__half>,
          "DeepSpeed self_attention backward with fp16 (CUDA)");
    m.def("create_self_attention_layer_fp32",
          &create_self_attention_layer<float>,
          "Create DeepSpeed self_attention self_attention Layer with fp32 (CUDA)");
    m.def("create_self_attention_layer_fp16",
          &create_self_attention_layer<__half>,
          "Create DeepSpeed self_attention self_attention Layer with fp16 (CUDA)");
    m.def("forward_mlp_fp32",
          &ds_mlp_forward<float>,
          "DeepSpeed mlp forward with fp32 (CUDA)");
    m.def("forward_mlp_fp16",
          &ds_mlp_forward<__half>,
          "DeepSpeed mlp forward with fp16 (CUDA)");
    m.def("backward_mlp_fp32",
          &ds_mlp_backward<float>,
          "DeepSpeed mlp backward with fp32 (CUDA)");
    m.def("backward_mlp_fp16",
          &ds_mlp_backward<__half>,
          "DeepSpeed mlp backward with fp16 (CUDA)");
    m.def("create_mlp_layer_fp32",
          &create_mlp_layer<float>,
          "Create DeepSpeed mlp Layer with fp32 (CUDA)");
    m.def("create_mlp_layer_fp16",
          &create_mlp_layer<__half>,
          "Create DeepSpeed mlp Layer with fp16 (CUDA)");
    m.def("forward_bias_residual_dropout_fp32",
          &ds_bias_residual_dropout_forward<float>,
          "DeepSpeed bias_residual_dropout forward with fp32 (CUDA)");
    m.def("forward_bias_residual_dropout_fp16",
          &ds_bias_residual_dropout_forward<__half>,
          "DeepSpeed bias_residual_dropout forward with fp16 (CUDA)");
    m.def("backward_bias_residual_dropout_fp32",
          &ds_bias_residual_dropout_backward<float>,
          "DeepSpeed bias_residual_dropout backward with fp32 (CUDA)");
    m.def("backward_bias_residual_dropout_fp16",
          &ds_bias_residual_dropout_backward<__half>,
          "DeepSpeed bias_residual_dropout backward with fp16 (CUDA)");
    m.def("create_bias_residual_dropout_layer_fp32",
          &create_bias_residual_dropout_layer<float>,
          "Create DeepSpeed bias_residual_dropout Layer with fp32 (CUDA)");
    m.def("create_bias_residual_dropout_layer_fp16",
          &create_bias_residual_dropout_layer<__half>,
          "Create DeepSpeed bias_residual_dropout Layer with fp16 (CUDA)");
    m.def("forward_LayerNormalize_fp32",
          &ds_LayerNormalize_forward<float>,
          "DeepSpeed LayerNormalize forward with fp32 (CUDA)");
    m.def("forward_LayerNormalize_fp16",
          &ds_LayerNormalize_forward<__half>,
          "DeepSpeed LayerNormalize forward with fp16 (CUDA)");
    m.def("backward_LayerNormalize_fp32",
          &ds_LayerNormalize_backward<float>,
          "DeepSpeed LayerNormalize backward with fp32 (CUDA)");
    m.def("backward_LayerNormalize_fp16",
          &ds_LayerNormalize_backward<__half>,
          "DeepSpeed LayerNormalize backward with fp16 (CUDA)");
    m.def("create_LayerNormalize_layer_fp32",
          &create_LayerNormalize_layer<float>,
          "Create DeepSpeed LayerNormalize Layer with fp32 (CUDA)");
    m.def("create_LayerNormalize_layer_fp16",
          &create_LayerNormalize_layer<__half>,
          "Create DeepSpeed LayerNormalize Layer with fp16 (CUDA)");
    m.def("store_random_state",
          &store_rand_state,
          "store random state");
    m.def("restore_random_state",
          &restore_rand_state,
          "restore random state");
}   
