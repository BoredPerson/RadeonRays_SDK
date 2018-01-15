/**********************************************************************
Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/
#include "bvh2.h"

#include <atomic>
#include <mutex>
#include <numeric>
#include <stack>

#define PARALLEL_BUILD

namespace RadeonRays
{
#ifdef __GNUC__
    #define clz(x) __builtin_clz(x)
    #define ctz(x) __builtin_ctz(x)
#else
    inline
    std::uint32_t popcnt(std::uint32_t x)
    {
        x -= ((x >> 1) & 0x55555555);
        x = (((x >> 2) & 0x33333333) + (x & 0x33333333));
        x = (((x >> 4) + x) & 0x0f0f0f0f);
        x += (x >> 8);
        x += (x >> 16);
        return x & 0x0000003f;
    }

    inline
    std::uint32_t clz(std::uint32_t x)
    {
        x |= (x >> 1);
        x |= (x >> 2);
        x |= (x >> 4);
        x |= (x >> 8);
        x |= (x >> 16);
        return 32 - popcnt(x);
    }

    inline
    std::uint32_t ctz(std::uint32_t x)
    {
        return popcnt((std::uint32_t)(x & -(int)x) - 1);
    }
#endif

    inline
    __m128 aabb_surface_area(__m128 pmin, __m128 pmax)
    {
        auto ext = _mm_sub_ps(pmax, pmin);
        auto xxy = _mm_shuffle_ps(ext, ext, _MM_SHUFFLE(3, 1, 0, 0));
        auto yzz = _mm_shuffle_ps(ext, ext, _MM_SHUFFLE(3, 2, 2, 1));
        return _mm_mul_ps(_mm_dp_ps(xxy, yzz, 0xff), _mm_set_ps(2.f, 2.f, 2.f, 2.f));
    }

    inline
    __m128 aabb_extents(__m128 pmin, __m128 pmax)
    {
        return _mm_sub_ps(pmax, pmin);
    }

    inline
    std::uint32_t aabb_max_extent_axis(__m128 pmin, __m128 pmax)
    {
        auto xyz = _mm_sub_ps(pmax, pmin);
        auto yzx = _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(3, 0, 2, 1));
        auto m0 = _mm_max_ps(xyz, yzx);
        auto m1 = _mm_shuffle_ps(m0, m0, _MM_SHUFFLE(3, 0, 2, 1));
        auto m2 = _mm_max_ps(m0, m1);
        auto cmp = _mm_cmpeq_ps(xyz, m2);
        return ctz(_mm_movemask_ps(cmp));
    }

    inline
    float mm_select(__m128 v, std::uint32_t index)
    {
        _MM_ALIGN16 float temp[4];
        _mm_store_ps(temp, v);
        return temp[index];
    }

    inline
    bool aabb_contains_point(
            float const* aabb_min,
            float const* aabb_max,
            float const* point)
    {
        return point[0] >= aabb_min[0] &&
               point[0] <= aabb_max[0] &&
               point[1] >= aabb_min[1] &&
               point[1] <= aabb_max[1] &&
               point[2] >= aabb_min[2] &&
               point[2] <= aabb_max[2];
    }

    struct Bvh2::SplitRequest
    {
        __m128 aabb_min;
        __m128 aabb_max;
        __m128 centroid_aabb_min;
        __m128 centroid_aabb_max;
        std::size_t start_index;
        std::size_t num_refs;
        std::uint32_t level;
        std::uint32_t index;
    };

    void Bvh2::BuildImpl(
        __m128 scene_min,
        __m128 scene_max,
        __m128 centroid_scene_min,
        __m128 centroid_scene_max,
        const float3 *aabb_min,
        const float3 *aabb_max,
        const float3 *aabb_centroid,
        const MetaDataArray &metadata,
        std::size_t num_aabbs)
    {
        RefArray refs(num_aabbs);
        std::iota(refs.begin(), refs.end(), 0);

        m_nodecount = 2 * num_aabbs - 1;
        m_nodes = reinterpret_cast<Node*>(
            Allocate(sizeof(Node) * m_nodecount, 16u));

        auto constexpr inf = std::numeric_limits<float>::infinity();
        auto m128_plus_inf = _mm_set_ps(inf, inf, inf, inf);
        auto m128_minus_inf = _mm_set_ps(-inf, -inf, -inf, -inf);

#ifndef PARALLEL_BUILD
        #error TODO: implement (gboisse)
#else
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic_bool shutdown = false;
        std::atomic_uint32_t num_refs_processed = 0;

        std::stack<SplitRequest> requests;

        requests.push(SplitRequest{
            scene_min,
            scene_max,
            centroid_scene_min,
            centroid_scene_max,
            0,
            num_aabbs,
            0u,
            0u
        });

        auto worker_thread = [&]()
        {
            thread_local std::stack<SplitRequest> local_requests;

            for (;;)
            {
                // Wait for signal
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&]() { return !requests.empty() || shutdown; });

                    if (shutdown) return;

                    local_requests.push(requests.top());
                    requests.pop();
                }

                _MM_ALIGN16 SplitRequest request;
                _MM_ALIGN16 SplitRequest request_left;
                _MM_ALIGN16 SplitRequest request_right;

                // Process local requests
                while (!local_requests.empty())
                {
                    request = local_requests.top();
                    local_requests.pop();

                    auto node_type = HandleRequest(
                        request,
                        aabb_min,
                        aabb_max,
                        aabb_centroid,
                        metadata,
                        refs,
                        num_aabbs,
                        request_left,
                        request_right);

                    if (node_type == kLeaf)
                    {
                        num_refs_processed += static_cast<std::uint32_t>(request.num_refs);
                        continue;
                    }

                    if (request_right.num_refs > 4096u)
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        requests.push(request_right);
                        cv.notify_one();
                    }
                    else
                    {
                        local_requests.push(request_right);
                    }

                    local_requests.push(request_left);
                }
            }
        };

        auto num_threads = std::thread::hardware_concurrency();
        std::vector<std::thread> threads(num_threads);

        for (auto i = 0u; i < num_threads; ++i)
        {
            threads[i] = std::move(std::thread(worker_thread));
            threads[i].detach();
        }

        while (num_refs_processed != num_aabbs)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        shutdown = true;
#endif
    }

    Bvh2::NodeType Bvh2::HandleRequest(
        const SplitRequest &request,
        const float3 *aabb_min,
        const float3 *aabb_max,
        const float3 *aabb_centroid,
        const MetaDataArray &metadata,
        RefArray &refs,
        std::size_t num_aabbs,
        SplitRequest &request_left,
        SplitRequest &request_right)
    {
        // Do we have enough primitives?
        if (request.num_refs <= kMaxLeafPrimitives)
        {
            EncodeLeaf(m_nodes[request.index], static_cast<std::uint32_t>(request.num_refs));
            for (auto i = 0u; i < request.num_refs; ++i)
            {
                auto face_data = metadata[refs[request.start_index + i]];
                SetPrimitive(
                    m_nodes[request.index],
                    i,
                    face_data);
            }
            return kLeaf;
        }

        // Otherwise, find split axis
        auto split_axis = aabb_max_extent_axis(
            request.centroid_aabb_min,
            request.centroid_aabb_max);

        auto split_axis_extent = mm_select(
            _mm_sub_ps(request.centroid_aabb_max,
                request.centroid_aabb_min),
            split_axis);

        auto split_value = mm_select(
            _mm_mul_ps(
                _mm_set_ps(0.5f, 0.5f, 0.5f, 0.5),
                _mm_add_ps(request.centroid_aabb_max,
                    request.centroid_aabb_min)),
            split_axis);

        auto split_idx = request.start_index;

        auto constexpr inf = std::numeric_limits<float>::infinity();
        auto m128_plus_inf = _mm_set_ps(inf, inf, inf, inf);
        auto m128_minus_inf = _mm_set_ps(-inf, -inf, -inf, -inf);

        auto lmin = m128_plus_inf;
        auto lmax = m128_minus_inf;
        auto rmin = m128_plus_inf;
        auto rmax = m128_minus_inf;

        auto lcmin = m128_plus_inf;
        auto lcmax = m128_minus_inf;
        auto rcmin = m128_plus_inf;
        auto rcmax = m128_minus_inf;

        // Partition the primitives
        if (split_axis_extent > 0.0f)
        {
            if (m_usesah && request.num_refs > kMinSAHPrimitives)
            {
                // TODO: implement SAH (gboisse)
            }

            auto first = request.start_index;
            auto last = request.start_index + request.num_refs;

            for (;;)
            {
                while ((first != last) &&
                    aabb_centroid[refs[first]][split_axis] < split_value)
                {
                    auto idx = refs[first];
                    lmin = _mm_min_ps(lmin, _mm_load_ps(&aabb_min[idx].x));
                    lmax = _mm_max_ps(lmax, _mm_load_ps(&aabb_max[idx].x));

                    auto c = _mm_load_ps(&aabb_centroid[idx].x);
                    lcmin = _mm_min_ps(lcmin, c);
                    lcmax = _mm_max_ps(lcmax, c);

                    ++first;
                }

                if (first == last--) break;

                auto idx = refs[first];
                rmin = _mm_min_ps(rmin, _mm_load_ps(&aabb_min[idx].x));
                rmax = _mm_max_ps(rmax, _mm_load_ps(&aabb_max[idx].x));

                auto c = _mm_load_ps(&aabb_centroid[idx].x);
                rcmin = _mm_min_ps(rcmin, c);
                rcmax = _mm_max_ps(rcmax, c);

                while ((first != last) &&
                    aabb_centroid[refs[last]][split_axis] >= split_value)
                {
                    auto idx = refs[last];
                    rmin = _mm_min_ps(rmin, _mm_load_ps(&aabb_min[idx].x));
                    rmax = _mm_max_ps(rmax, _mm_load_ps(&aabb_max[idx].x));

                    auto c = _mm_load_ps(&aabb_centroid[idx].x);
                    rcmin = _mm_min_ps(rcmin, c);
                    rcmax = _mm_max_ps(rcmax, c);

                    --last;
                }

                if (first == last) break;

                idx = refs[last];
                lmin = _mm_min_ps(lmin, _mm_load_ps(&aabb_min[idx].x));
                lmax = _mm_max_ps(lmax, _mm_load_ps(&aabb_max[idx].x));

                c = _mm_load_ps(&aabb_centroid[idx].x);
                lcmin = _mm_min_ps(lcmin, c);
                lcmax = _mm_max_ps(lcmax, c);

                std::swap(refs[first++], refs[last]);
            }

            split_idx = first;
        }

        if (split_idx == request.start_index ||
            split_idx == request.start_index + request.num_refs)
        {
            split_idx = request.start_index + (request.num_refs >> 1);

            lmin = m128_plus_inf;
            lmax = m128_minus_inf;
            rmin = m128_plus_inf;
            rmax = m128_minus_inf;

            lcmin = m128_plus_inf;
            lcmax = m128_minus_inf;
            rcmin = m128_plus_inf;
            rcmax = m128_minus_inf;

            for (auto i = request.start_index; i < split_idx; ++i)
            {
                auto idx = refs[i];
                lmin = _mm_min_ps(lmin, _mm_load_ps(&aabb_min[idx].x));
                lmax = _mm_max_ps(lmax, _mm_load_ps(&aabb_max[idx].x));

                auto c = _mm_load_ps(&aabb_centroid[idx].x);
                lcmin = _mm_min_ps(lcmin, c);
                lcmax = _mm_max_ps(lcmax, c);
            }

            for (auto i = split_idx; i < request.start_index + request.num_refs; ++i)
            {
                auto idx = refs[i];
                rmin = _mm_min_ps(rmin, _mm_load_ps(&aabb_min[idx].x));
                rmax = _mm_max_ps(rmax, _mm_load_ps(&aabb_max[idx].x));

                auto c = _mm_load_ps(&aabb_centroid[idx].x);
                rcmin = _mm_min_ps(rcmin, c);
                rcmax = _mm_max_ps(rcmax, c);
            }
        }

        // Populate child requests
        request_left.aabb_min = lmin;
        request_left.aabb_max = lmax;
        request_left.centroid_aabb_min = lcmin;
        request_left.centroid_aabb_max = lcmax;
        request_left.start_index = request.start_index;
        request_left.num_refs = split_idx - request.start_index;
        request_left.level = request.level + 1;
        request_left.index = request.index + 1;

        request_right.aabb_min = rmin;
        request_right.aabb_max = rmax;
        request_right.centroid_aabb_min = rcmin;
        request_right.centroid_aabb_max = rcmax;
        request_right.start_index = split_idx;
        request_right.num_refs = request.num_refs - request_left.num_refs;
        request_right.level = request.level + 1;
        request_right.index = static_cast<std::uint32_t>(request.index + request_left.num_refs * 2);

        // Create internal node
        EncodeInternal(
            m_nodes[request.index],
            request.aabb_min,
            request.aabb_max,
            request_left.index,
            request_right.index);

        return kInternal;
    }
}
