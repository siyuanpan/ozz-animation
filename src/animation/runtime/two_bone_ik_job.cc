//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/runtime/two_bone_ik_job.h"

#include <cassert>

#include "ozz/base/log.h"
#include "ozz/base/maths/simd_quaternion.h"

namespace ozz {
namespace animation {

TwoBoneIKJob::TwoBoneIKJob()
    : handle(math::simd_float4::zero()),
      pole_vector(math::simd_float4::y_axis()),
      start_joint(NULL),
      mid_joint(NULL),
      end_joint(NULL),
      start_joint_correction(NULL),
      mid_joint_correction(NULL) {}

bool TwoBoneIKJob::Validate() const {
  bool valid = true;
  valid &= start_joint && mid_joint && end_joint;
  valid &= start_joint_correction && mid_joint_correction;
  return valid;
}

bool TwoBoneIKJob::Run() const {
  if (!Validate()) {
    return false;
  }

  using namespace ozz::math;

  const SimdFloat4 zero = simd_float4::zero();
  const SimdFloat4 one = simd_float4::one();
  const SimdFloat4 half = simd_float4::Load1(.5f);
  const SimdInt4 mask_sign = simd_int4::mask_sign();

  // Computes inverse matrices required to change to start and mid spaces.
  const Float4x4 inv_start_joint = Invert(*start_joint);
  const Float4x4 inv_mid_joint = Invert(*mid_joint);

  // Transform some positions to mid joint space (_ms)
  const SimdFloat4 start_ms =
      TransformPoint(inv_mid_joint, start_joint->cols[3]);
  const SimdFloat4 end_ms = TransformPoint(inv_mid_joint, end_joint->cols[3]);

  // Transform some positions to start joint space (_ss)
  const SimdFloat4 mid_ss = TransformPoint(inv_start_joint, mid_joint->cols[3]);
  const SimdFloat4 end_ss = TransformPoint(inv_start_joint, end_joint->cols[3]);
  const SimdFloat4 handle_ss = TransformPoint(inv_start_joint, handle);
  const SimdFloat4 pole_ss = TransformVector(inv_start_joint, pole_vector);

  // Computes bones vectors and length in mid and start spaces.
  // Start joint position will be treated as 0 because all joints are
  // expressed in start joint space.
  const SimdFloat4 start_mid_ms = -start_ms;
  const SimdFloat4& mid_end_ms = end_ms;
  const SimdFloat4& start_mid_ss = mid_ss;
  const SimdFloat4 mid_end_ss = end_ss - mid_ss;
  const SimdFloat4& start_end_ss = end_ss;
  const SimdFloat4& start_handle_ss = handle_ss;
  const SimdFloat4 start_mid_ss_len2 = Length3Sqr(start_mid_ss);
  const SimdFloat4 mid_end_ss_len2 = Length3Sqr(mid_end_ss);
  const SimdFloat4 start_end_ss_len2 = Length3Sqr(start_end_ss);
  const SimdFloat4 start_handle_ss_len2 = Length3Sqr(start_handle_ss);

  // Calculate mid_rot_local quaternion which solves for the mid_ss joint
  // rotation.
  // --------------------------------------------------------------------------

  // Computes expected angle at mid_ss joint, using law of cosine (generalized
  // Pythagorean).
  // c^2 = a^2 + b^2 - 2ab cosC
  // cosC = (a^2 + b^2 - c^2) / 2ab
  // Computes both corrected and initial mid joint angles
  // cosine within a single SimdFloat4 (corrected is x component, initial is y).
  const SimdFloat4 start_mid_end_sum_ss_len2 =
      start_mid_ss_len2 + mid_end_ss_len2;
  const SimdFloat4 start_mid_end_ss_half_rlen =
      half * SplatX(RSqrtEstX(start_mid_ss_len2 * mid_end_ss_len2));
  // Cos value needs to be clamped, as it will exit expected range if
  // start_handle_ss_len2 is longer than the triangle can be (start_mid_ss +
  // mid_end_ss).
  const SimdFloat4 mid_cos_angles =
      Clamp(-one,
            (SplatX(start_mid_end_sum_ss_len2) -
             SetY(start_handle_ss_len2, start_end_ss_len2)) *
                start_mid_end_ss_half_rlen,
            one);

  // Computes corrected and initial angles difference.
  // Using the formulas for the differences of angles (below) and building
  // the quaternion form cosine avoids computing the 2 ACos here, and the
  // Sin and Cos in quaternion construction.
  // cos(A - B) = cosA * cosB + sinA * sinB
  // cos(A - B) = cosA * cosB + Sqrt((1 - cosA^2) * (1 - cosB^2))
  const SimdFloat4 one_minus_mid_cos_angles2 =
      one - mid_cos_angles * mid_cos_angles;
  const SimdFloat4 mid_cos_angle_diff_unclamped =
      mid_cos_angles * SplatY(mid_cos_angles) +
      SqrtX(Max(zero,
                one_minus_mid_cos_angles2 * SplatY(one_minus_mid_cos_angles2)));
  const SimdFloat4 mid_cos_angle_diff =
      Clamp(-one, mid_cos_angle_diff_unclamped, one);

  // Calculate mid joint axis
  // TODO safe ?
  const SimdFloat4 mid_axis_ms = Normalize3(Cross3(start_mid_ms, mid_end_ms));

  // Flip rotation direction if distance to handle is longer than initial
  // distance to end_ss (aka opposite angle).
  const SimdInt4 mid_axis_flip = SplatX(CmpLt(start_end_ss_len2, start_handle_ss_len2));
  const SimdFloat4 mid_axis_ms_flipped = Xor(mid_axis_ms, And(mid_axis_flip, mask_sign));

  // Mid joint rotation.
  const SimdQuaternion mid_rot_ms = SimdQuaternion::FromAxisCosAngle(
      mid_axis_ms_flipped, mid_cos_angle_diff);

  // Calculates end_to_handle_rot_ss quaternion which solves for effector
  // rotating onto the handle.
  // -----------------------------------------------------------------

  // start_mid_ss with quaternion mid_rot_ms applied.
  const SimdFloat4 mid_end_ss_corrected = TransformVector(
      inv_start_joint,
      TransformVector(*mid_joint, TransformVector(mid_rot_ms, mid_end_ms)));
  const SimdFloat4 start_end_ss_corrected = start_mid_ss + mid_end_ss_corrected;

  // Quaternion for rotating the effector onto the handle
  const SimdQuaternion end_to_handle_rot_ss =
      SimdQuaternion::FromVectors(start_end_ss_corrected, start_handle_ss);

  // Calculates rotate_plane_rot quaternion which aligns joint chain plane to
  // the reference plane (pole vector).
  // -------------------------------------------------

  // Computes each plane normal.
  const ozz::math::SimdFloat4 ref_plane_normal_ss =
      Cross3(start_handle_ss, pole_ss);
  const ozz::math::SimdFloat4 ref_plane_normal_ss_len2 =
      ozz::math::Length3Sqr(ref_plane_normal_ss);
  const ozz::math::SimdFloat4 joint_plane_normal_ss =
      TransformVector(end_to_handle_rot_ss, Cross3(start_end_ss, start_mid_ss));
  const ozz::math::SimdFloat4 joint_plane_normal_ss_len2 =
      ozz::math::Length3Sqr(joint_plane_normal_ss);
  // Computes all reciprocal square roots at once.
  const SimdFloat4 rsqrts =
      RSqrtEstNR(SetZ(SetY(start_handle_ss_len2, ref_plane_normal_ss_len2),
                      joint_plane_normal_ss_len2));

  // Computes angle cosine between the 2 normalized normals.
  const SimdFloat4 rotate_plane_cos_angle =
      ozz::math::Dot3(ref_plane_normal_ss * SplatY(rsqrts),
                      joint_plane_normal_ss * SplatZ(rsqrts));

  // Computes rotation axis, which is either start_handle_ss or
  // -start_handle_ss depending on rotation direction.
  // TODO start_handle_ss_len2 == 0
  const SimdInt4 start_axis_flip =
      And(SplatX(Dot3(joint_plane_normal_ss, pole_ss)), mask_sign);
  const SimdFloat4 rotate_plane_axis =
      Xor(start_handle_ss, start_axis_flip) * SplatX(rsqrts);

  // Builds quaternion along rotation axis.
  const SimdQuaternion rotate_plane_ss = SimdQuaternion::FromAxisCosAngle(
      rotate_plane_axis, Clamp(-one, rotate_plane_cos_angle, one));

  // Output start and mid joint rotation corrections.
  *mid_joint_correction = mid_rot_ms;
  *start_joint_correction = rotate_plane_ss * end_to_handle_rot_ss;

  return true;
}
}  // namespace animation
}  // namespace ozz
