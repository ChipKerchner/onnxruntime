// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "attention_base.h"
#include "attention_helper.h"

#include "core/common/common.h"
#include "contrib_ops/cpu/bert/attention_common.h"
#include "core/common/safeint.h"
#include "core/framework/op_kernel.h"

namespace onnxruntime {
namespace contrib {

class GQAAttentionBase : public AttentionBase {
 protected:
  GQAAttentionBase(const OpKernelInfo& info, bool require_same_hidden_size)
      : AttentionBase(info, require_same_hidden_size) {}

  int local_window_size_;
  bool do_rotary_;
  bool rotary_interleaved_;

  template <typename T>
  Status ApplyAttention(const T* Q,                                 // Q data with shape BxNxSxH
                        const T* K,                                 // K data with shape BxN_kvxSxH
                        const T* V,                                 // V data with shape BxN_kvxSxH
                        const Tensor* past_key,                     // past K input tensor (if not using past state)
                        const Tensor* past_value,                   // past V input tensor (if not using past state)
                        Tensor* output,                             // output tensor
                        Tensor* present_key,                        // present K output tensor (if separating present KV)
                        Tensor* present_value,                      // present V output tensor (if separating present KV)
                        const Tensor* seqlens_k,                    // past sequence lengths tensor
                        GroupQueryAttentionParameters& parameters,  // attention parameters
                        AllocatorPtr allocator,                     // allocator for temporary tensors
                        OpKernelContext* context) const {
    const int batch_size = parameters.batch_size;
    const int sequence_length = parameters.sequence_length;
    const int head_size = parameters.head_size;
    const int hidden_size = parameters.hidden_size;
    const bool packed_qkv = parameters.is_packed_qkv;

    auto* tp = context->GetOperatorThreadPool();

    int seqlen_past_kv_cache = 0;
    if (past_key != nullptr && past_value != nullptr) {
      seqlen_past_kv_cache = static_cast<int>(past_key->Shape().GetDims()[2]);
    }
    int seqlen_present_kv_cache = static_cast<int>(present_key->Shape().GetDims()[2]);

    // Compute the attention score.
    size_t bytes = SafeInt<size_t>(batch_size) * num_heads_ * sequence_length * seqlen_present_kv_cache * sizeof(T);
    auto attention_probs = allocator->Alloc(bytes);
    BufferUniquePtr scratch_buffer(attention_probs, BufferDeleter(allocator));

    void* mask_data = nullptr;
    size_t mask_data_bytes = SafeInt<size_t>(batch_size) * sequence_length * seqlen_present_kv_cache * sizeof(T);
    mask_data = allocator->Alloc(mask_data_bytes);
    memset(mask_data, 0, mask_data_bytes);
    BufferUniquePtr mask_data_buffer(mask_data, BufferDeleter(allocator));

    const T* past_key_data = past_key != nullptr ? past_key->Data<T>() : nullptr;
    T* present_key_data = present_key != nullptr ? present_key->MutableData<T>() : nullptr;
    const T* past_value_data = past_value != nullptr ? past_value->Data<T>() : nullptr;
    T* present_value_data = present_value != nullptr ? present_value->MutableData<T>() : nullptr;

    bool past_present_share_buffer = past_key_data == present_key_data && past_value_data == present_value_data;

    const T* k = packed_qkv ? Q + num_heads_ * sequence_length * head_size : K;
    ComputeAttentionProbs<T>(static_cast<T*>(attention_probs), Q, k,
                             seqlens_k->Data<int32_t>(), static_cast<T*>(mask_data),
                             batch_size, sequence_length, seqlen_past_kv_cache, seqlen_present_kv_cache,
                             head_size, past_key_data, present_key_data, past_present_share_buffer, packed_qkv, tp);

    // Compute the attentionScore * Value: out_tmp(B, N, S, H_v) = attention_probs(B, N, S, T) x V(B, N, T, H_v)
    auto out_tmp_data =
        allocator->Alloc(SafeInt<size_t>(batch_size) * num_heads_ * sequence_length * head_size * sizeof(T));
    BufferUniquePtr out_tmp_buffer(out_tmp_data, BufferDeleter(allocator));

    const T* v = packed_qkv ? Q + (num_heads_ + kv_num_heads_) * sequence_length * head_size : V;
    ComputeVxAttentionScore(output->MutableData<T>(), static_cast<T*>(out_tmp_data), static_cast<T*>(attention_probs),
                            v, seqlens_k->Data<int32_t>(), batch_size, sequence_length, seqlen_past_kv_cache,
                            seqlen_present_kv_cache, head_size, hidden_size, past_value_data, present_value_data,
                            past_present_share_buffer, packed_qkv, tp);

    return Status::OK();
  }

 private:
  // Helper function to compute the attention probs. It does 2 things:
  //  attention_probs(B, N, S, T) = 1/sqrt(H) x Q(B, N, S, H) x K'(B, N, T, H -> B, N, H, T) +
  //                                1 x mask_data(B, N, S, T)
  //  attention_probs(B, N, S, T) = Softmax(attention_probs)
  template <typename T>
  void ComputeAttentionProbs(T* attention_probs,                  // output buffer with size BxNxSxT
                             const T* Q,                          // Q data. Its size is BxNxSxH
                             const T* K,                          // k data. Its size is BxNxLxH
                             const int32_t* seqlens_k,            // past sequence lengths tensor
                             T* mask_data,                        // buffer for mask data.
                             int batch_size,                      // batch size of self-attention
                             int sequence_length,                 // sequence length of self-attention (S)
                             int past_buffer_sequence_length,     // sequence length of past state
                             int present_buffer_sequence_length,  // sequence length of present state
                             int head_size,                       // head size of self-attention
                             const T* past_key,                   // past key only (if not using past state)
                             T* present_key,                      // present key only (if not using present state)
                             bool past_present_share_buffer,      // whether present key and value share the same buffer
                             bool packed_qkv,                     // whether Q, K, V are packed
                             ThreadPool* tp) const {              // thread pool
    const bool is_prompt = sequence_length != 1;
    const int packed_batch_stride = packed_qkv ? (num_heads_ + 2 * kv_num_heads_) * sequence_length * head_size : 0;
    const int kv_num_heads_factor = num_heads_ / kv_num_heads_;
    const size_t q_input_chunk_length = static_cast<size_t>(sequence_length) * head_size;                      // S x H
    const size_t kv_input_chunk_length = static_cast<size_t>(sequence_length) * head_size;                     // L x H
    const size_t past_buff_chunk_length = static_cast<size_t>(past_buffer_sequence_length) * head_size;        // L x H
    const size_t present_buff_chunk_length = static_cast<size_t>(present_buffer_sequence_length) * head_size;  // T x H

    PrepareMaskGQA(mask_data, batch_size, sequence_length, present_buffer_sequence_length, local_window_size_, seqlens_k);

    const int loop_len = batch_size * num_heads_;
    const float alpha = scale_ == 0.0f ? 1.0f / sqrt(static_cast<float>(head_size)) : scale_;

    // TODO: cost might differ for gqa because of right padding and total_sequence_length being sequence dependent
    TensorOpCost unit_cost;
    const size_t probs_matrix_bytes = SafeInt<size_t>(sequence_length) * present_buffer_sequence_length * sizeof(T);
    unit_cost.compute_cycles = static_cast<double>(2 * sequence_length * head_size * present_buffer_sequence_length);
    unit_cost.bytes_loaded = static_cast<double>((sequence_length + present_buffer_sequence_length) * head_size * sizeof(T));
    unit_cost.bytes_stored = static_cast<double>(probs_matrix_bytes);

    unit_cost.bytes_loaded += static_cast<double>(probs_matrix_bytes);
    unit_cost.bytes_stored += static_cast<double>(probs_matrix_bytes);

    if (present_key) {
      double bytes_to_copy_key = static_cast<double>(sizeof(T) * present_buff_chunk_length);
      unit_cost.bytes_loaded += bytes_to_copy_key;
      unit_cost.bytes_stored += bytes_to_copy_key;
    }

    ThreadPool::TryParallelFor(tp, loop_len, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
      for (std::ptrdiff_t i = begin; i != end; ++i) {
        const int batch_index = static_cast<int>(i) / num_heads_;
        const int head_index = static_cast<int>(i) % num_heads_;
        const int past_seqlen = sequence_length == 1 ? static_cast<int>(seqlens_k[batch_index]) : past_buffer_sequence_length;
        const size_t past_chunk_length = static_cast<size_t>(past_seqlen) * head_size;

        const int output_offset = static_cast<int>(i) * sequence_length * present_buffer_sequence_length;
        const int mask_offset = batch_index * sequence_length * present_buffer_sequence_length;
        T* output = attention_probs + output_offset;

        // Broadcast mask data: (Bx)SxT -> (BxNx)SxT
        // TODO: mask after present_sequence_length
        memcpy(output,
               mask_data + mask_offset,
               probs_matrix_bytes);

        const T* k;
        if (packed_qkv) {
          k = K + packed_batch_stride * batch_index + kv_input_chunk_length * (head_index / kv_num_heads_factor);
        } else {
          k = K + kv_input_chunk_length * (i / kv_num_heads_factor);
        }
        if (nullptr != present_key) {
          k = ConcatStateChunkGQA(past_key, k, present_key, present_buff_chunk_length, past_buff_chunk_length,
                                  past_chunk_length, kv_input_chunk_length, is_prompt, past_present_share_buffer,
                                  i / kv_num_heads_factor);
        }

        // TODO: CblasTrans stuff what do?
        // Compute Q*K' + AttentionMask
        //                     original                 transposed             each iteration
        // A: Q                (B x N x) S x H          (B x N x) S x H        S x H
        // B: K'               (B x N x) T x H          (B x N x) H x T        H x T
        // C: attention_probs  (B x N x) S x T          (B x N x) S x T        S x T
        const T* q;
        if (packed_qkv) {
          q = Q + packed_batch_stride * batch_index + q_input_chunk_length * head_index;
        } else {
          q = Q + q_input_chunk_length * i;
        }
        math::Gemm<T, ThreadPool>(CblasNoTrans, CblasTrans, sequence_length, present_buffer_sequence_length, head_size, alpha,
                                  q, k, mask_data != nullptr ? 1.0f : 0.0f, output, nullptr);
      }
    });

    // attention_probs(B, N, S, T) = Softmax(attention_probs)
    const int N = batch_size * num_heads_ * sequence_length;
    const int D = present_buffer_sequence_length;
    ComputeAttentionSoftmaxInplace(attention_probs, N, D, tp);
  }

  template <typename T>
  void ComputeVxAttentionScore(T* output,                           // buffer for the result with size BxSxNxH
                               T* tmp_buffer,                       // buffer for temp use with size is BxNxSxH
                               const T* attention_probs,            // Attention probs with size BxNxSxT
                               const T* V,                          // V value with size BxN_kvxSxH
                               const int32_t* seqlens_k,            // past sequence lengths tensor
                               int batch_size,                      // batch size
                               int sequence_length,                 // sequence length
                               int past_buffer_sequence_length,     // sequence length in past state
                               int present_buffer_sequence_length,  // sequence length in past state
                               int head_size,                       // head size of Q, K, V
                               int hidden_size,                     // hidden size of Output
                               const T* past_value,                 // past value only (if not using past state)
                               T* present_value,                    // present value only (if not using present state)
                               bool past_present_share_buffer,      // whether present key and value share the same buffer
                               bool packed_qkv,                     // whether Q, K, V are packed
                               ThreadPool* tp) const {
    const bool is_prompt = sequence_length != 1;
    const int packed_batch_stride = packed_qkv ? (num_heads_ + 2 * kv_num_heads_) * sequence_length * head_size : 0;
    const int kv_num_heads_factor = num_heads_ / kv_num_heads_;
    // TODO: what is with these being ptrdiff_t?
    const ptrdiff_t q_input_chunk_length = SafeInt<ptrdiff_t>(sequence_length) * head_size;                    // S x H
    const ptrdiff_t kv_input_chunk_length = SafeInt<ptrdiff_t>(sequence_length) * head_size;                   // L x H
    const size_t past_buff_chunk_length = static_cast<size_t>(past_buffer_sequence_length) * head_size;        // L x H
    const size_t present_buff_chunk_length = static_cast<size_t>(present_buffer_sequence_length) * head_size;  // T x H

    // The cost of Gemm
    TensorOpCost unit_cost;
    unit_cost.compute_cycles = static_cast<double>(2 * sequence_length * head_size * present_buffer_sequence_length);
    unit_cost.bytes_loaded = static_cast<double>((sequence_length + head_size) * present_buffer_sequence_length * sizeof(T));
    unit_cost.bytes_stored = static_cast<double>(sequence_length * head_size * sizeof(T));

    if (present_value) {
      double bytes_to_copy_value = static_cast<double>(present_buff_chunk_length * sizeof(T));
      unit_cost.bytes_loaded += bytes_to_copy_value;
      unit_cost.bytes_stored += bytes_to_copy_value;
    }

    const size_t bytes_to_copy_trans = SafeInt<size_t>(head_size) * sizeof(T);
    double bytes_to_copy_trans_all = static_cast<double>(sequence_length * bytes_to_copy_trans);
    unit_cost.bytes_loaded += bytes_to_copy_trans_all;
    unit_cost.bytes_stored += bytes_to_copy_trans_all;

    ThreadPool::TryParallelFor(tp, SafeInt<ptrdiff_t>(batch_size) * num_heads_, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
      for (std::ptrdiff_t i = begin; i != end; ++i) {
        const int batch_index = static_cast<int>(i / num_heads_);
        const int head_index = static_cast<int>(i % num_heads_);
        const int past_seqlen = sequence_length == 1 ? static_cast<int>(seqlens_k[batch_index]) : past_buffer_sequence_length;
        const size_t past_chunk_length = static_cast<size_t>(past_seqlen) * head_size;

        const T* v;
        if (packed_qkv) {
          v = V + packed_batch_stride * batch_index + kv_input_chunk_length * (head_index / kv_num_heads_factor);
        } else {
          v = V + kv_input_chunk_length * (i / kv_num_heads_factor);
        }
        if (nullptr != present_value) {
          v = ConcatStateChunkGQA(past_value, v, present_value, present_buff_chunk_length, past_buff_chunk_length,
                                  past_chunk_length, kv_input_chunk_length, is_prompt, past_present_share_buffer,
                                  i / kv_num_heads_factor);
        }

        T* current_tmp_data = reinterpret_cast<T*>(tmp_buffer) + q_input_chunk_length * i;
        ptrdiff_t attention_probs_offset = SafeInt<ptrdiff_t>(sequence_length) * present_buffer_sequence_length * i;
        math::MatMul<T>(sequence_length, head_size, present_buffer_sequence_length,
                        attention_probs + attention_probs_offset,
                        v, current_tmp_data, nullptr);

        // Transpose: out(B, S, N, H_v) -> out_tmp(B, N, S, H_v)
        T* src = current_tmp_data;
        ptrdiff_t dest_offset = (SafeInt<ptrdiff_t>(batch_index) * sequence_length * num_heads_ + head_index) * head_size;
        T* dest = output + dest_offset;
        for (int j = 0; j < sequence_length; j++) {
          memcpy(dest, src, bytes_to_copy_trans);
          src += head_size;
          dest += hidden_size;
        }
      }
    });
  }
};

}  // namespace contrib
}  // namespace onnxruntime
