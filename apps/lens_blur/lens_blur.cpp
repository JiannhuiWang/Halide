#include "Halide.h"

using namespace Halide;

#include <iostream>
#include <limits>

#include "benchmark.h"
#include "halide_image_io.h"

using namespace Halide::Tools;

using std::vector;

Var x, y, z, c;

// Downsample with a 1 3 3 1 filter
Func downsample(Func f) {
    Func downx, downy;

    downx(x, y, _) = (f(2*x-1, y, _) + 3.0f * (f(2*x, y, _) + f(2*x+1, y, _)) + f(2*x+2, y, _)) / 8.0f;
    downy(x, y, _) = (downx(x, 2*y-1, _) + 3.0f * (downx(x, 2*y, _) + downx(x, 2*y+1, _)) + downx(x, 2*y+2, _)) / 8.0f;

    return downy;
}

// Upsample using bilinear interpolation
Func upsample(Func f) {
    Func upx, upy;

    upx(x, y, _) = 0.25f * f((x/2) - 1 + 2*(x % 2), y, _) + 0.75f * f(x/2, y, _);
    upy(x, y, _) = 0.25f * upx(x, (y/2) - 1 + 2*(y % 2), _) + 0.75f * upx(x, y/2, _);

    return upy;

}


int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Usage:\n\t./lens_blur left.png right.png out.png\n";
        return 1;
    }

    // The number of displacements to consider
    const int slices = 32;

    // The depth to focus on
    const int focus_depth = 13;

    // The increase in blur radius with misfocus depth
    const float blur_radius_scale = 0.5;

    // The number of samples of the aperture to use
    const int aperture_samples = 32;

    const int maximum_blur_radius = std::max(slices - focus_depth, focus_depth) * blur_radius_scale;

    ImageParam left_im(UInt(8), 3), right_im(UInt(8), 3);

    Func left = BoundaryConditions::repeat_edge(left_im);
    Func right = BoundaryConditions::repeat_edge(right_im);

    Func diff;
    diff(x, y, z, c) = min(absd(left(x, y, c), right(x + 2*z, y, c)),
                           absd(left(x, y, c), right(x + 2*z + 1, y, c)));

    Func cost;
    cost(x, y, z) = (pow(cast<float>(diff(x, y, z, 0)), 2) +
                     pow(cast<float>(diff(x, y, z, 1)), 2) +
                     pow(cast<float>(diff(x, y, z, 2)), 2));

    // Compute confidence of cost estimate at each pixel by taking the
    // variance across the stack.
    Func cost_confidence;
    {
        RDom r(0, slices);
        Expr a = sum(pow(cost(x, y, r), 2)) / slices;
        Expr b = pow(sum(cost(x, y, r) / slices), 2);
        cost_confidence(x, y) = a - b;
    }

    // Do a push-pull thing to blur the cost volume with an
    // exponential-decay type thing to inpaint over regions with low
    // confidence.
    Func cost_pyramid_push[8];
    cost_pyramid_push[0](x, y, z, c) =
        select(c == 0, cost(x, y, z) * cost_confidence(x, y), cost_confidence(x, y));
    Expr w = left_im.width(), h = left_im.height();
    for (int i = 1; i < 8; i++) {
        cost_pyramid_push[i](x, y, z, c) = downsample(cost_pyramid_push[i-1])(x, y, z, c);
        w /= 2;
        h /= 2;
        cost_pyramid_push[i] = BoundaryConditions::repeat_edge(cost_pyramid_push[i], {{0, w}, {0, h}});
    }

    Func cost_pyramid_pull[8];
    cost_pyramid_pull[7](x, y, z, c) = cost_pyramid_push[7](x, y, z, c);
    for (int i = 6; i >= 0; i--) {
        cost_pyramid_pull[i](x, y, z, c) = lerp(upsample(cost_pyramid_pull[i+1])(x, y, z, c),
                                                cost_pyramid_push[i](x, y, z, c),
                                                0.5f);
    }

    Func filtered_cost;
    filtered_cost(x, y, z) = (cost_pyramid_pull[0](x, y, z, 0) /
                              cost_pyramid_pull[0](x, y, z, 1));

    // Assume the minimum cost slice is the correct depth.
    Func depth;
    {
        RDom r(0, slices);
        depth(x, y) = argmin(filtered_cost(x, y, r))[0];
    }

    Func bokeh_radius;
    bokeh_radius(x, y) = abs(depth(x, y) - focus_depth) * blur_radius_scale;

    Func bokeh_radius_squared;
    bokeh_radius_squared(x, y) = pow(bokeh_radius(x, y), 2);

    // Take a max filter of the bokeh radius to determine the
    // worst-case bokeh radius to consider at each pixel. Makes the
    // sampling more efficient below.
    Func worst_case_bokeh_radius_y, worst_case_bokeh_radius;
    {
        RDom r(-maximum_blur_radius, 2*maximum_blur_radius+1);
        worst_case_bokeh_radius_y(x, y) = maximum(bokeh_radius(x, y + r));
        worst_case_bokeh_radius(x, y) = maximum(worst_case_bokeh_radius_y(x + r, y));
    }

    Func input_with_alpha;
    input_with_alpha(x, y, c) = select(c == 0, cast<float>(left(x, y, 0)),
                                       c == 1, cast<float>(left(x, y, 1)),
                                       c == 2, cast<float>(left(x, y, 2)),
                                       255.0f);

    // Render a blurred image
    Func output;
    output(x, y, c) = input_with_alpha(x, y, c);

    // The sample locations are a random function of x, y, and sample
    // number (not c).
    Expr worst_radius = worst_case_bokeh_radius(x, y);
    Expr sample_u = (random_float() - 0.5f) * 2 * worst_radius;
    Expr sample_v = (random_float() - 0.5f) * 2 * worst_radius;
    sample_u = clamp(cast<int>(sample_u), -maximum_blur_radius, maximum_blur_radius);
    sample_v = clamp(cast<int>(sample_v), -maximum_blur_radius, maximum_blur_radius);
    Func sample_locations;
    sample_locations(x, y, z) = {sample_u, sample_v};

    RDom s(0, aperture_samples);
    sample_u = sample_locations(x, y, z)[0];
    sample_v = sample_locations(x, y, z)[1];
    Expr sample_x = x + sample_u, sample_y = y + sample_v;
    Expr r_squared = sample_u * sample_u + sample_v * sample_v;

    // We use this sample if it's from a pixel whose bokeh influences
    // this output pixel. Here's a crude approximation that ignores
    // some subtleties of occlusion edges and inpaints behind objects.
    Expr sample_is_within_bokeh_of_this_pixel =
        r_squared < bokeh_radius_squared(x, y);

    Expr this_pixel_is_within_bokeh_of_sample =
        r_squared < bokeh_radius_squared(sample_x, sample_y);

    Expr sample_is_in_front_of_this_pixel =
        depth(sample_x, sample_y) < depth(x, y);

    Func sample_weight;
    sample_weight(x, y, z) =
        select((sample_is_within_bokeh_of_this_pixel ||
                sample_is_in_front_of_this_pixel) &&
               this_pixel_is_within_bokeh_of_sample,
               1.0f, 0.0f);

    sample_x = x + sample_locations(x, y, s)[0];
    sample_y = y + sample_locations(x, y, s)[1];
    output(x, y, c) += sample_weight(x, y, s) * input_with_alpha(sample_x, sample_y, c);

    // Normalize
    Func final;
    final(x, y, c) = output(x, y, c) / output(x, y, 3);

    Image<uint8_t> in_l = load_image(argv[1]);
    Image<uint8_t> in_r = load_image(argv[2]);
    final.bound(x, 0, in_l.width()).bound(y, 0, in_l.height()).bound(c, 0, 3);

    int schedule = atoi(argv[4]);
    switch(schedule) {
    case 0:
    {
        // Write down time and runtime here. I ran with 16 threads on my z620.
    }
    case 1:
    {
        // ...
    }
    break;
    default:
        break;
    }

    // Run it

    left_im.set(in_l);
    right_im.set(in_r);
    Image<float> out(in_l.width(), in_l.height(), 3);
    Target target = get_target_from_environment();
    if (schedule == -1) {
        final.compile_jit(target, true);
    } else {
        final.compile_jit(target);
    }

    std::cout << "Running... " << std::endl;
    double best = benchmark(5, 5, [&]() { final.realize(out); });
    std::cout << " took " << best * 1e3 << " msec." << std::endl;

    save_image(out, argv[3]);

    return 0;
}