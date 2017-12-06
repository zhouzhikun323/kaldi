// nnet3/nnet-normalize-component.h

// Copyright 2011-2013  Karel Vesely
//           2012-2015  Johns Hopkins University (author: Daniel Povey)
//                2013  Xiaohui Zhang
//           2014-2015  Vijayaditya Peddinti
//           2014-2015  Guoguo Chen
//                2015  Daniel Galvez
//                2015  Tom Ko

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#ifndef KALDI_NNET3_NNET_NORMALIZE_COMPONENT_H_
#define KALDI_NNET3_NNET_NORMALIZE_COMPONENT_H_

#include "nnet3/nnet-common.h"
#include "nnet3/nnet-component-itf.h"
#include "nnet3/natural-gradient-online.h"
#include <iostream>

namespace kaldi {
namespace nnet3 {

/// @file  nnet-normalize-component.h
///
///   This file contains declarations of components that in one way or
///   another normalize their input: NormalizeComponent, BatchNormComponent,
///   and MemoryNormComponent.

/*
   Implements the function:

         y = x * (sqrt(dim(x)) * target-rms) / |x|

    where |x| is the 2-norm of the vector x.  I.e. its output is its input
    scaled such that the root-mean-square values of its elements equals
    target-rms.  (As a special case, if the input is zero, it outputs zero).

    Note: if you specify add-log-stddev=true, it adds an extra element to
     y which equals log(|x| / sqrt(dim(x))).


   Configuration values accepted:
      dim, or input-dim    Input dimension of this component, e.g. 1024.
                           Will be the same as the output dimension if add-log-stddev=false.
      block-dim            Defaults to 'dim' you may specify a nonzero divisor
                           of 'dim'.  In this case the input dimension will
                           be interpreted as blocks of dimension 'block-dim'
                           to which the nonlinearity described above is applied
                           separately.
      add-log-stddev       You can set this to true to add an extra output
                           dimension which will equal |x| / sqrt(dim(x)).
                           If block-dim is specified, this is done per block.
      target-rms           This defaults to 1.0, but if set it to another
                           (nonzero) value, the output will be scaled by this
                           factor.
 */
class NormalizeComponent: public Component {
 public:
  explicit NormalizeComponent(const NormalizeComponent &other);

  virtual int32 Properties() const {
    return kSimpleComponent|kBackpropNeedsInput|kBackpropAdds|
        (add_log_stddev_ ? 0 : kPropagateInPlace|kBackpropInPlace) |
        (block_dim_ != input_dim_ ? kInputContiguous|kOutputContiguous : 0);
  }
  NormalizeComponent() { }
  virtual std::string Type() const { return "NormalizeComponent"; }
  virtual void InitFromConfig(ConfigLine *cfl);
  virtual Component* Copy() const { return new NormalizeComponent(*this); }
  virtual void* Propagate(const ComponentPrecomputedIndexes *indexes,
                          const CuMatrixBase<BaseFloat> &in,
                          CuMatrixBase<BaseFloat> *out) const;
  virtual void Backprop(const std::string &debug_info,
                        const ComponentPrecomputedIndexes *indexes,
                        const CuMatrixBase<BaseFloat> &in_value,
                        const CuMatrixBase<BaseFloat> &, // out_value
                        const CuMatrixBase<BaseFloat> &out_deriv,
                        void *memo,
                        Component *to_update,
                        CuMatrixBase<BaseFloat> *in_deriv) const;

  virtual void Read(std::istream &is, bool binary);
  virtual void Write(std::ostream &os, bool binary) const;
  virtual int32 InputDim() const { return input_dim_; }
  virtual int32 OutputDim() const {
    return (input_dim_ + (add_log_stddev_ ? (input_dim_ / block_dim_) : 0));
  }
  virtual std::string Info() const;
 private:
  NormalizeComponent &operator = (const NormalizeComponent &other); // Disallow.
  enum { kExpSquaredNormFloor = -66 };
  // kSquaredNormFloor is about 0.7e-20.  We need a value that's exactly representable in
  // float and whose inverse square root is also exactly representable
  // in float (hence, an even power of two).
  static const BaseFloat kSquaredNormFloor;
  int32 input_dim_;
  int32 block_dim_;
  BaseFloat target_rms_; // The target rms for outputs, default 1.0.

  bool add_log_stddev_; // If true, log(max(epsi, sqrt(row_in^T row_in / D)))
                        // is an extra dimension of the output.
};


/*
  BatchNormComponent

  This implements batch normalization; for each dimension of the
  input it normalizes the data to be zero-mean, unit-variance.  You
  can set the block-dim configuration value to implement spatial
  batch normalization, see the comment for the variable.

  If you want to combine this with the trainable offset and scale that the
  original BatchNorm paper used, then follow this by the
  ScaleAndOffsetComponent.

  It's a simple component (uses the kSimpleComponent flag), but it is unusual in
  that it will give different results if you call it on half the matrix at a
  time.  Most of the time this would be pretty harmless, so we still return the
  kSimpleComponent flag.  We may have to modify the test code a little to
  account for this, or possibly remove the kSimpleComponent flag.  In some sense
  each output Index depends on every input Index, but putting those dependencies
  explicitly into the dependency-tracking framework as a GeneralComponent
  would be very impractical and might lead to a lot of unnecessary things being
  computed.  You have to be a bit careful where you put this component, and understand
  what you're doing e.g. putting it in the path of a recurrence is a bit problematic
  if the minibatch size is small.

    Accepted configuration values:
           dim          Dimension of the input and output
           block-dim    Defaults to 'dim', but may be set to a nonzero divisor
                        of 'dim'.  In this case, each block of dimension 'block-dim'
                        is treated like a separate row of the input matrix, which
                        means that the stats from n'th element of each
                        block are pooled into one class, for each n.a
           epsilon      Small term added to the variance that is used to prevent
                        division by zero
           target-rms   This defaults to 1.0, but if set, for instance, to 2.0,
                        it will normalize the standard deviation of the output to
                        2.0. 'target-stddev' might be a more suitable name, but this
                        was chosen for consistency with NormalizeComponent.
 */
class BatchNormComponent: public Component {
 public:

  BatchNormComponent() { }

  // call this with 'true' to set 'test mode' where the batch normalization is
  // done with stored stats.  There won't normally be any need to specially
  // accumulate these stats; they are stored as a matter of course on each
  // iteration of training, as for NonlinearComponents, and we'll use the stats
  // from the most recent [script-level] iteration.
  void SetTestMode(bool test_mode);

  // constructor using another component
  BatchNormComponent(const BatchNormComponent &other);

  virtual int32 InputDim() const { return dim_; }
  virtual int32 OutputDim() const { return dim_; }

  virtual std::string Info() const;
  virtual void InitFromConfig(ConfigLine *cfl);
  virtual std::string Type() const { return "BatchNormComponent"; }
  virtual int32 Properties() const {
    // If the block-dim is less than the dim, we need the input and output
    // matrices to be contiguous (stride==num-cols), as we'll be reshaping
    // internally.  This is not much of a cost, because this will be used
    // in convnets where we have to do this anyway.
    return kSimpleComponent|kBackpropNeedsOutput|kPropagateInPlace|
        kBackpropInPlace|
        (block_dim_ < dim_ ? kInputContiguous|kOutputContiguous : 0)|
        (test_mode_ ? 0 : kUsesMemo|kStoresStats);
  }
  virtual void* Propagate(const ComponentPrecomputedIndexes *indexes,
                         const CuMatrixBase<BaseFloat> &in,
                         CuMatrixBase<BaseFloat> *out) const;
  virtual void Backprop(const std::string &debug_info,
                        const ComponentPrecomputedIndexes *indexes,
                        const CuMatrixBase<BaseFloat> &in_value,
                        const CuMatrixBase<BaseFloat> &out_value,
                        const CuMatrixBase<BaseFloat> &out_deriv,
                        void *memo,
                        Component *, // to_update,
                        CuMatrixBase<BaseFloat> *in_deriv) const;

  virtual void Read(std::istream &is, bool binary); // This Read function
  // requires that the Component has the correct type.

  /// Write component to stream
  virtual void Write(std::ostream &os, bool binary) const;
  virtual Component* Copy() const { return new BatchNormComponent(*this); }

  virtual void Scale(BaseFloat scale);
  virtual void Add(BaseFloat alpha, const Component &other);
  virtual void ZeroStats();


  virtual void DeleteMemo(void *memo) const { delete static_cast<Memo*>(memo); }

  virtual void StoreStats(const CuMatrixBase<BaseFloat> &in_value,
                          const CuMatrixBase<BaseFloat> &out_value,
                          void *memo);

  // Members specific to this component type.
  // Note: the offset and scale will only be nonempty in 'test mode'.
  const CuVector<BaseFloat> &Offset() const { return offset_; }
  const CuVector<BaseFloat> &Scale() const { return scale_; }

 private:

  struct Memo {
    // number of frames (after any reshaping).
    int32 num_frames;
    // 'sum_sumsq_scale' is of dimension 4 by block_dim_:
    // Row 0 = mean = the mean of the rows of the input
    // Row 1 = uvar = the uncentered variance of the input (= sumsq / num_frames).
    // Row 2 = scale = the scale of the renormalization, which is
    // Row 3 is used as a temporary in Backprop.
    //    the inverse stddev of the input (modified by epsilon_,
    //    see the Propagate function.
    CuMatrix<BaseFloat> mean_uvar_scale;
  };

  void Check() const;

  // this function is used in a couple of places; it turns the raw stats into
  // the offset/scale term of a normalizing transform.
  static void ComputeOffsetAndScale(double count,
                                    BaseFloat epsilon,
                                    const Vector<double> &stats_sum,
                                    const Vector<double> &stats_sumsq,
                                    Vector<BaseFloat> *offset,
                                    Vector<BaseFloat> *scale);
  // computes derived parameters offset_ and scale_.
  void ComputeDerived();

  // Dimension of the input and output.
  int32 dim_;
  // This would normally be the same as dim_, but if it's less (and it must be >
  // 0 and must divide dim_), then each separate block of the input of dimension
  // 'block_dim_' is treated like a separate frame for the purposes of
  // normalization.  This can be used to implement spatial batch normalization
  // for convolutional setups-- assuming the filter-dim has stride 1, which it
  // always will in the new code in nnet-convolutional-component.h.
  int32 block_dim_;

  // Used to avoid exact-zero variances, epsilon has the dimension of a
  // covariance.
  BaseFloat epsilon_;

  // This value will normally be 1.0, which is the default, but you can set it
  // to other values as a way to control how fast the following layer learns
  // (smaller -> slower).  The same config exists in NormalizeComponent.
  BaseFloat target_rms_;

  // This is true if we want the batch normalization to operate in 'test mode'
  // meaning the data mean and stddev used for the normalization are fixed
  // quantities based on previously accumulated stats.  Note: the stats we use
  // for this are based on the same 'StoreStats' mechanism as we use for
  // components like SigmoidComponent and ReluComponent; we'll be using
  // the stats from the most recent [script-level] iteration of training.
  bool test_mode_;


  // total count of stats stored by StoreStats().
  double count_;
  // sum-of-data component of stats of input data.
  CuVector<double> stats_sum_;
  // sum-of-squared component of stats of input data.
  CuVector<double> stats_sumsq_;

  // offset_ and scale_ are derived from stats_sum_ and stats_sumsq_; they
  // dictate the transform that is done in 'test mode'.  They are set only when
  // reading the model from disk and when calling SetTestMode(true); they are
  // resized to empty when the stats are updated, to ensure that out-of-date
  // values are not kept around.
  CuVector<BaseFloat> offset_;
  CuVector<BaseFloat> scale_;
};


/*
  MemoryNormComponent

  MemoryNormComponent is like batch normalization, except the stats
  are accumulated as a weighted sum over past minibatches (if this is
  not the first minibatch), instead of over the current minibatch.

  You can use it in the same way you would normally use BatchNormComponent.

  Accepted configuration values:
         dim          Dimension of the input and output
         block-dim    Defaults to 'dim', but may be set to a nonzero divisor
                      of 'dim'.  In this case, each block of dimension 'block-dim'
                      is treated like a separate row of the input matrix, which
                      means that the stats from n'th element of each
                      block are pooled into one class, for each n.a
         epsilon      Small term added to the variance that is used to prevent
                      division by zero
         target-rms   This defaults to 1.0, but if set, for instance, to 2.0,
                      it will normalize the standard deviation of the output to
                      2.0. 'target-stddev' might be a more suitable name, but this
                      was chosen for consistency with NormalizeComponent.
         include-indirect-derivative  This defaults to true, which means we
                      include the (smaller) derivative term that comes via the
                      mean and variance estimation.  You might want to set this to
                      false for testing purposes.
 */
class MemoryNormComponent: public Component {
 public:

  MemoryNormComponent() { }

  // constructor using another component
  MemoryNormComponent(const MemoryNormComponent &other);

  virtual int32 InputDim() const { return dim_; }
  virtual int32 OutputDim() const { return dim_; }

  virtual std::string Info() const;
  virtual void InitFromConfig(ConfigLine *cfl);
  virtual std::string Type() const { return "MemoryNormComponent"; }
  virtual int32 Properties() const {
    // If the block-dim is less than the dim, we need the input and output
    // matrices to be contiguous (stride==num-cols), as we'll be reshaping
    // internally.  This is not much of a cost, because this will be used
    // in convnets where we have to do this anyway.
    bool iid = include_indirect_derivative_;
    return kSimpleComponent|kPropagateInPlace|kBackpropInPlace|
        (test_mode_ ? 0 : kUsesMemo|kStoresStats|(iid?kBackpropNeedsOutput:0))|
        (block_dim_ < dim_ ? kInputContiguous|kOutputContiguous : 0);

  }

  // Call this function to set 'test mode' to true or false.  In test
  // mode the stats are frozen and will not be updated.
  void SetTestMode(bool test_mode);


  virtual void* Propagate(const ComponentPrecomputedIndexes *indexes,
                         const CuMatrixBase<BaseFloat> &in,
                         CuMatrixBase<BaseFloat> *out) const;

  /// The backprop function.  In addition to propagating the input back to
  /// 'in_deriv', if supplied, this function also updates, in 'to_update',
  /// backward_count_ and the rows named 'y_deriv' and 'y_deriv_y' of
  /// data_, and also the derived quantities 'x_deriv' and 'scale_deriv'
  /// of data_.
  /// (note: in training, 'to_update' will point to delta_nnet_, and later these
  /// stats get added to nnet_ via Add())
  virtual void Backprop(const std::string &debug_info,
                        const ComponentPrecomputedIndexes *indexes,
                        const CuMatrixBase<BaseFloat> &, // in_value
                        const CuMatrixBase<BaseFloat> &out_value,
                        const CuMatrixBase<BaseFloat> &out_deriv,
                        void *memo,
                        Component *to_update,
                        CuMatrixBase<BaseFloat> *in_deriv) const;

  virtual void Read(std::istream &is, bool binary); // This Read function
  // requires that the Component has the correct type.

  /// Write component to stream
  virtual void Write(std::ostream &os, bool binary) const;
  virtual Component* Copy() const { return new MemoryNormComponent(*this); }

  // Note: if you scale by a negative number it will set stats to zero
  // rather than allow a negative stats count.
  virtual void Scale(BaseFloat scale);
  // Note: if you try to add with negative coefficient (as in backstitch), it
  // will do nothing.
  virtual void Add(BaseFloat alpha, const Component &other);
  virtual void ZeroStats();

  virtual void DeleteMemo(void *memo) const { delete static_cast<Memo*>(memo); }

  /// This function updates stats_count_, the rows named 'x_mean', 'x_uvar'
  /// of data_, and also the derived quantities stored in the rows named
  /// 'scale', 'x_deriv' and 'scale_deriv' of data_.
  /// (note: in training, this is called on the delta_nnet_, and later
  /// the stats get added to nnet_ via Add())
  virtual void StoreStats(const CuMatrixBase<BaseFloat> &, // in_value
                          const CuMatrixBase<BaseFloat> &, // out_value
                          void *memo);

 private:

  struct Memo {
    // The number of frames (after any reshaping; so in general it will
    // be the original NumRows() of the matrix, times dim_ / block_dim_).
    int32 num_frames;
    // 'data' is of dimension 5 by block_dim_.
    // Row 0, which we'll call 'x_sum', is the sum of the rows of the
    //  input data.
    // Row 1, which we'll call 'x_sumsq', is the sum of the rows of the
    //   elementwise square of the input data matrix.
    // Row 2,3,4 are 'scale', 'x_deriv', 'scale_deriv', which
    //         are just copies of the corresponding values in
    //         MemoryNormComponent::data_ (from the const nnet, the one we're
    //         training), and which will have been copied from there when this
    //         object was created.  However if stats_count_ was <= 0 when this
    //         object was created (first minibatch), then 'scale'
    //         will be set to the mean and inverse-stddev implied by the stats
    //         'sum' and 'sumsq', and 'x_deriv' and 'scale_deriv' will be zero.
    //         This is so that it does something sensible on the very first
    //         minibatch we train.  The reason why we copy these quantities here
    //         is because in the backprop phase we feel it would be better to
    //         use the same values that were used in the forward propagation,
    //         instead of the possibly-updated values that might exist when
    //         Backprop() is called.  It's actually not clear whether this is
    //         necessary.
    CuMatrix<BaseFloat> data;

    // This is set to true if we have the 'indirect' terms in the derivative,
    // relating to the 'x_deriv' and 'scale_deriv' terms in 'data'.  If false,
    // we save some computation.
    bool has_indirect_terms;
  };


  /// This piece of code, which has been broken out from Propagate(), computes
  /// the memo.  Expects in.NumCols() == block_dim_.  It should only be called
  /// if test_mode_ is false.
  Memo *GetMemo(const CuMatrixBase<BaseFloat> &in) const;

  /// This function computes certain members of data_ that are derived:
  /// specifically, rows 4, 5 and 6, which are called 'scale', 'x_deriv' and
  /// 'scale_deriv'.
  void ComputeDerived();

  void Check() const;

  // this function is used in a couple of places; it turns the raw stats into
  // the offset/scale term of a normalizing transform.
  static void ComputeOffsetAndScale(BaseFloat count,
                                    BaseFloat epsilon,
                                    const Vector<BaseFloat> &stats_sum,
                                    const Vector<BaseFloat> &stats_sumsq,
                                    Vector<BaseFloat> *offset,
                                    Vector<BaseFloat> *scale);

  // Dimension of the input and output.
  int32 dim_;

  // block_dim_ would normally be the same as dim_, but if it's less (and it
  // must be > 0 and must divide dim_), then each separate block of the input of
  // dimension 'block_dim_' is treated like a separate frame for the purposes of
  // normalization.  This can be used to implement spatial batch normalization
  // for convolutional setups-- assuming the filter-dim has stride 1, which it
  // always will in the new code in nnet-convolutional-component.h.
  int32 block_dim_;

  // Used to avoid exact-zero variances, epsilon has the dimension of a
  // covariance.
  BaseFloat epsilon_;

  // This controls the dynamic range of the output.  At 1.0 which is the
  // default, the output has unit standard deviation, but you can set it to
  // other values.  The same config exists in NormalizeComponent.
  BaseFloat target_rms_;

  // If true, we include the smaller indirect part of the derivative, that comes
  // via the stats estimation.  This is included mostly for testing purposes; we
  // expect this will normally be true.
  bool include_indirect_derivative_;

  // If test_mode_ is set, no stats will be accumulated.  It's an error if
  // test_mode_ is set and the data count is zero, and you try to propagate.
  bool test_mode_;

  // The total count of stats stored by StoreStats(), and which are represented
  // in x_mean = data_.Row(0) and x_uvar = data_.Row(1).  We never allow this to
  // become less than zero, even if people do unexpected things with Add() and
  // Scale().
  BaseFloat stats_count_;

  // backward_count_ is the total count of stats accumulated during backprop,
  // and represents the count correspondsing to the stats in 'y_deriv' and
  // 'y_deriv_y'.  It is expected to be either zero or the same as stats_count_,
  // in most circumstances, depending whether you were doing backprop or just
  // inference-- but we don't enforce this because there may be situations where
  // this is not the case.
  //
  // We never allow this to become less than zero, even if people do unexpected
  // things with Add() and Scale().
  BaseFloat backward_count_;

  // We store data_ as a single matrix because it enables certain operations
  // to be done using fewer kernels, but it contains various different quantities,
  // which we'll describe below as if they were separate variables.
  // data_ is of dimension 6 by block_dim_.
  CuMatrix<BaseFloat> data_;
  // data_.Row(0) is 'x_mean', which is the decaying moving-average of
  //             input data x; or zero if stats_count_ is zero.
  // data_.Row(1) is 'x_uvar', which is the decaying moving-average of
  //             input data x^2 or zero if stats_count_ is zero.
  // data_.Row(2) is 'y_deriv', which is the decaying moving-average
  //           derivative of the objective w.r.t. the output y; or
  //           zero if backward_count_ is zero.
  // data_.Row(3) is 'y_deriv_y', which  the decaying moving average
  //           of the product of the output times (the derivative of the
  //           objective w.r.t. the output); or zero if backward_count_
  //           is zero.
  //
  // The quantities below are derived from the stats above.
  //
  // data_.Row(4) is 'scale', which is the inverse square root of the
  //            covariance computed from x_mean and x_uvar (plus epsilon),
  //            or zero if stats_count_ is zero.
  // data_.Row(5) is 'x_deriv', which is the negative of the average derivative
  //           (per frame) of the objective function w.r.t the input x (just the
  //           part that comes via the derivative w.r.t. the x mean).
  //           'x_deriv' equals 'y_deriv' times 'scale'.
  // data_.Row(6) is 'scale_deriv', which relates to the part of the
  //           derivative w.r.t. the input that comes from the objf
  //           derivative w.r.t. the scale.  It equals scale * y_deriv_y.
};





} // namespace nnet3
} // namespace kaldi


#endif
