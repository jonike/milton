// milton.h
// (c) Copyright 2015 by Sergio Gonzalez


#include "libserg/defaults.h"

// ==== Order matters. ====
//
//      We are compiling a big-blob of C code. Not multiple files. Everything
//      is #include'd only once.
//
//      There are only two places where we can add #include directives:
//          - This file.
//          - Platform-dependent *.c files.
//          - Be crystal clear on why these rules are broken when they are
//          broken. (if ever)
//
//      Every other file should assume that it knows about every function it
//      needs to call and every struct it uses. It is our responsibility to
//      include things in the right order in this file to make the program
//      compile.
//
// ========================

#include <math.h>  // powf
#include <float.h>

#include "libserg/gl_helpers.h"
#include "vector.generated.h"  // Generated by metaprogram

#include "utils.h"
#include "color.h"
#include "canvas.h"


typedef struct MiltonGLState_s
{
    GLuint quad_program;
    GLuint texture;
    GLuint quad_vao;
} MiltonGLState;

typedef enum MiltonMode_s
{
    MiltonMode_NONE =                   ( 0 ),

    MiltonMode_ERASER =                 ( 1 << 0 ),
    MiltonMode_BRUSH =                  ( 1 << 1 ),
    MiltonMode_REQUEST_QUALITY_REDRAW = ( 1 << 2 ),
} MiltonMode;

typedef struct RenderQueue_s RenderQueue;

typedef struct MiltonState_s
{
    i32     max_width;              // Dimensions of the raster
    i32     max_height;
    u8      bytes_per_pixel;
    u8*     raster_buffers[2];      // Double buffering, for render jobs that may not finish.
    i32     raster_buffer_index;

    // The screen is rendered in tiles
    // Each tile is rendered in blocks of size (block_width*block_width).
    i32     blocks_per_tile;
    i32     block_width;

    MiltonGLState* gl;

    ColorManagement cm;

    ColorPicker picker;

    Brush   brush;
    Brush   eraser_brush;
    i32     brush_size;  // In screen pixels

    b32 canvas_blocked;  // When interacting with the UI.

    CanvasView* view;

    v2i     last_raster_input;  // Last input point. Used to determine area to update.

    Stroke  working_stroke;

    Stroke  strokes[4096];  // TODO: Create a deque to store arbitrary number of strokes.
    i32     num_strokes;

    i32     num_redos;

    MiltonMode current_mode;

    i32             num_render_workers;
    RenderQueue*    render_queue;


    // Heap
    Arena*      root_arena;         // Persistent memory.
    Arena*      transient_arena;    // Gets reset after every call to milton_update().
    Arena*      render_worker_arenas;

} MiltonState;

typedef enum
{
    MiltonInputFlags_NONE,
    MiltonInputFlags_FULL_REFRESH    = ( 1 << 0 ),
    MiltonInputFlags_RESET           = ( 1 << 1 ),
    MiltonInputFlags_END_STROKE      = ( 1 << 2 ),
    MiltonInputFlags_UNDO            = ( 1 << 3 ),
    MiltonInputFlags_REDO            = ( 1 << 4 ),
    MiltonInputFlags_SET_MODE_ERASER = ( 1 << 5 ),
    MiltonInputFlags_SET_MODE_BRUSH  = ( 1 << 6 ),
    MiltonInputFlags_FAST_DRAW       = ( 1 << 7 ),
} MiltonInputFlags;

typedef struct MiltonInput_s
{
    MiltonInputFlags flags;

    v2i* point;
    int  scale;
    v2i  pan_delta;
} MiltonInput;

#include "rasterizer.h"

static void milton_gl_backend_draw(MiltonState* milton_state)
{
    MiltonGLState* gl = milton_state->gl;
    u8* raster_buffer = milton_state->raster_buffers[milton_state->raster_buffer_index];
    glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA,
            milton_state->view->screen_size.w, milton_state->view->screen_size.h,
            0, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)raster_buffer);
    glUseProgram(gl->quad_program);
    glBindVertexArray(gl->quad_vao);
    GLCHK (glDrawArrays (GL_TRIANGLE_FAN, 0, 4) );
}

static void milton_gl_backend_init(MiltonState* milton_state)
{
    // Init quad program
    {
        const char* shader_contents[2];

        shader_contents[0] =
            "#version 330\n"
            "#extension GL_ARB_explicit_uniform_location : enable\n"
            "layout(location = 0) in vec2 position;\n"
            "\n"
            "out vec2 coord;\n"
            "\n"
            "void main()\n"
            "{\n"
            "   coord = (position + vec2(1,1))/2;\n"
            "   coord.y = 1 - coord.y;"
            "   // direct to clip space. must be in [-1, 1]^2\n"
            "   gl_Position = vec4(position, 0.0, 1.0);\n"
            "}\n";


        shader_contents[1] =
            "#version 330\n"
            "#extension GL_ARB_explicit_uniform_location : enable\n"
            "\n"
            "layout(location = 1) uniform sampler2D buffer;\n"
            "in vec2 coord;\n"
            "out vec4 out_color;\n"
            "\n"
            "void main(void)\n"
            "{\n"
            // TODO: Why RGB to BGR?
            "   out_color = texture(buffer, coord).bgra;"
            "}\n";

        GLuint shader_objects[2] = {0};
        for ( int i = 0; i < 2; ++i )
        {
            GLuint shader_type = (i == 0) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
            shader_objects[i] = gl_compile_shader(shader_contents[i], shader_type);
        }
        milton_state->gl->quad_program = glCreateProgram();
        gl_link_program(milton_state->gl->quad_program, shader_objects, 2);

        glUseProgram(milton_state->gl->quad_program);
        glUniform1i(1, 0 /*GL_TEXTURE0*/);
    }

    // Create texture
    {
        GLCHK (glActiveTexture (GL_TEXTURE0) );
        // Create texture
        GLCHK (glGenTextures   (1, &milton_state->gl->texture));
        GLCHK (glBindTexture   (GL_TEXTURE_2D, milton_state->gl->texture));

        // Note for the future: These are needed.
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));
    }
    // Create quad
    {
#define u -1.0f
        // full
        GLfloat vert_data[] =
        {
            -u, +u,
            -u, -u,
            +u, -u,
            +u, +u,
        };
#undef u
        GLCHK (glGenVertexArrays(1, &milton_state->gl->quad_vao));
        GLCHK (glBindVertexArray(milton_state->gl->quad_vao));

        GLuint vbo;
        GLCHK (glGenBuffers(1, &vbo));
        GLCHK (glBindBuffer(GL_ARRAY_BUFFER, vbo));

        GLCHK (glBufferData (GL_ARRAY_BUFFER, sizeof(vert_data), vert_data, GL_STATIC_DRAW));
        GLCHK (glEnableVertexAttribArray (0) );
        GLCHK (glVertexAttribPointer     (/*attrib location*/0,
                    /*size*/2, GL_FLOAT, /*normalize*/GL_FALSE, /*stride*/0, /*ptr*/0));
    }
}

#ifndef NDEBUG
static void milton_startup_tests()
{
    v3f rgb = hsv_to_rgb((v3f){ 0 });
    assert(rgb.r == 0 &&
           rgb.g == 0 &&
           rgb.b == 0);
    rgb = hsv_to_rgb((v3f){ 0, 0, 1.0 });
    assert(rgb.r == 1 &&
           rgb.g == 1 &&
           rgb.b == 1);
    rgb = hsv_to_rgb((v3f){ 120, 1.0f, 0.5f });
    assert(rgb.r == 0 &&
           rgb.g == 0.5f &&
           rgb.b == 0);
    rgb = hsv_to_rgb((v3f){ 0, 1.0f, 1.0f });
    assert(rgb.r == 1.0f &&
           rgb.g == 0 &&
           rgb.b == 0);
}

static void milton_blend_tests()
{
    v4f a = { 1,0,0, 0.5f };
    v4f b = { 0,1,0, 0.5f };
    v4f blend = blend_v4f(a, b);
    assert (blend.r > 0);
}
#endif

static void milton_init(MiltonState* milton_state, i32 max_width , i32 max_height)
{

    // Initialize render queue
    milton_state->render_queue = arena_alloc_elem(milton_state->root_arena, RenderQueue);
    {
        milton_state->render_queue->work_available      = SDL_CreateSemaphore(0);
        milton_state->render_queue->completed_semaphore = SDL_CreateSemaphore(0);
        milton_state->render_queue->mutex               = SDL_CreateMutex();
    }

    // Even with hyper-threading, it's better to have extra workers.
    milton_state->num_render_workers = SDL_GetCPUCount() * 2;

    milton_log("[DEBUG]: Creating %d render workers.\n", milton_state->num_render_workers);

    milton_state->render_worker_arenas = arena_alloc_array(milton_state->root_arena,
                                                           milton_state->num_render_workers,
                                                           Arena);

    assert (milton_state->num_render_workers);

    for (i32 i = 0; i < milton_state->num_render_workers; ++i)
    {
        WorkerParams* params = arena_alloc_elem(milton_state->root_arena, WorkerParams);
        {
            *params = (WorkerParams) { milton_state, i };
        }

        static const size_t render_worker_memory = 16 * 1024 * 1024;
        milton_state->render_worker_arenas[i] = arena_spawn(milton_state->root_arena,
                                                            render_worker_memory);

        SDL_CreateThread((SDL_ThreadFunction)render_worker, "Worker Thread", (void*)params);
    }


#ifndef NDEBUG
    milton_startup_tests();
    milton_blend_tests();

#endif
    // Allocate enough memory for the maximum possible supported resolution. As
    // of now, it seems like future 8k displays will adopt this resolution.
    milton_state->max_width        = max_width;
    milton_state->max_height       = max_height;
    milton_state->bytes_per_pixel  = 4;
    milton_state->num_strokes      = 0;

    milton_state->current_mode = MiltonMode_BRUSH;

    i64 raster_buffer_size =
        milton_state->max_width * milton_state->max_height * milton_state->bytes_per_pixel;
    milton_state->raster_buffers[0] =
        arena_alloc_array(milton_state->root_arena, raster_buffer_size, u8);
    milton_state->raster_buffers[1] =
        arena_alloc_array(milton_state->root_arena, raster_buffer_size, u8);

    milton_state->gl = arena_alloc_elem(milton_state->root_arena, MiltonGLState);

    milton_state->blocks_per_tile = 16;
    milton_state->block_width = 32;

    color_init(&milton_state->cm);

    // Set the view
    {
        milton_state->view = arena_alloc_elem(milton_state->root_arena, CanvasView);
        // view->screen_size is set by the platform abstraction layer.
        // view->screen_center is also set there.
        milton_state->view->scale = (1 << 10);
        milton_state->view->downsampling_factor = 1;
        milton_state->view->canvas_tile_radius = 1024 * 1024 * 512;
#if 0
        milton_state->view->rotation = 0;
        for (int d = 0; d < 360; d++)
        {
            f32 r = deegrees_to_radians(d);
            f32 c = cosf(r);
            f32 s = sinf(r);
            milton_state->view->cos_sin_table[d][0] = c;
            milton_state->view->cos_sin_table[d][1] = s;
        }
#endif
    }

    // Init picker
    {
        i32 bound_radius_px = 100;
        f32 wheel_half_width = 12;
        milton_state->picker.center = (v2i){ 120, 120 };
        milton_state->picker.bound_radius_px = bound_radius_px;
        milton_state->picker.wheel_half_width = wheel_half_width;
        milton_state->picker.wheel_radius = (f32)bound_radius_px - 5.0f - wheel_half_width;
        milton_state->picker.hsv = (v3f){ 0.0f, 1.0f, 0.7f };
        Rect bounds;
        bounds.left = milton_state->picker.center.x - bound_radius_px;
        bounds.right = milton_state->picker.center.x + bound_radius_px;
        bounds.top = milton_state->picker.center.y - bound_radius_px;
        bounds.bottom = milton_state->picker.center.y + bound_radius_px;
        milton_state->picker.bounds = bounds;
        milton_state->picker.pixels = arena_alloc_array(
                milton_state->root_arena, (4 * bound_radius_px * bound_radius_px), u32);
        picker_update(&milton_state->picker,
                (v2i){
                milton_state->picker.center.x + (int)(milton_state->picker.wheel_radius),
                milton_state->picker.center.y
                });
    }
    milton_state->brush_size = 10;

    Brush brush = { 0 };
    {
        brush.radius = milton_state->brush_size * milton_state->view->scale;
#if 1
        brush.alpha = 0.5f;
#else
        brush.alpha = 1.0f;
#endif
        brush.color = hsv_to_rgb(milton_state->picker.hsv);
    }
    milton_state->brush = brush;

    milton_state->eraser_brush = (Brush)
    {
        .radius = milton_state->brush.radius,
        .alpha = 1.0f,
        .color = (v3f) { 1, 1, 1 },
    };

    milton_gl_backend_init(milton_state);
}

inline b32 is_user_drawing(MiltonState* milton_state)
{
    b32 result = milton_state->working_stroke.num_points > 0;
    return result;
}

static void milton_resize(MiltonState* milton_state, v2i pan_delta, v2i new_screen_size)
{
    if (new_screen_size.w < milton_state->max_width &&
        new_screen_size.h < milton_state->max_height)
    {

        milton_state->view->screen_size = new_screen_size;
        milton_state->view->screen_center = invscale_v2i(milton_state->view->screen_size, 2);

        // Add delta to pan vector
        v2i pan_vector = add_v2i(milton_state->view->pan_vector,
                                 scale_v2i(pan_delta, milton_state->view->scale));

        while (pan_vector.x > milton_state->view->canvas_tile_radius)
        {
            milton_state->view->canvas_tile_focus.x++;
            pan_vector.x -= milton_state->view->canvas_tile_radius;
        }
        while (pan_vector.x <= -milton_state->view->canvas_tile_radius)
        {
            milton_state->view->canvas_tile_focus.x--;
            pan_vector.x += milton_state->view->canvas_tile_radius;
        }
        while (pan_vector.y > milton_state->view->canvas_tile_radius)
        {
            milton_state->view->canvas_tile_focus.y++;
            pan_vector.y -= milton_state->view->canvas_tile_radius;
        }
        while (pan_vector.y <= -milton_state->view->canvas_tile_radius)
        {
            milton_state->view->canvas_tile_focus.y--;
            pan_vector.y += milton_state->view->canvas_tile_radius;
        }
        milton_state->view->pan_vector = pan_vector;
    }
    else
    {
        assert(!"DEBUG: new screen size is more than we can handle.");
    }
}

// Our "render loop" inner function.
static void milton_update(MiltonState* milton_state, MiltonInput* input)
{
    arena_reset(milton_state->transient_arena);

    MiltonRenderFlags render_flags = MiltonRenderFlags_none;

    if (input->flags & MiltonInputFlags_FAST_DRAW)
    {
        milton_state->view->downsampling_factor = 2;
        milton_state->current_mode |= MiltonMode_REQUEST_QUALITY_REDRAW;
    }
    else
    {
        milton_state->view->downsampling_factor = 1;
        if (milton_state->current_mode & MiltonMode_REQUEST_QUALITY_REDRAW)
        {
            milton_state->current_mode ^= MiltonMode_REQUEST_QUALITY_REDRAW;
            render_flags |= MiltonRenderFlags_full_redraw;
        }
    }

    if (input->flags & MiltonInputFlags_FULL_REFRESH)
    {
        render_flags |= MiltonRenderFlags_full_redraw;
    }

    if (input->scale)
    {
        render_flags |= MiltonRenderFlags_full_redraw;

// Sensible
#if 1
        static f32 scale_factor = 1.3f;
        static i32 view_scale_limit = 10000;
// Debug
#else
        static f32 scale_factor = 1.5f;
        static i32 view_scale_limit = 1000000;
#endif

        static b32 debug_scale_lock = false;
        if (!debug_scale_lock && input->scale > 0 && milton_state->view->scale >= 2)
        {
            milton_state->view->scale = (i32)(milton_state->view->scale / scale_factor);
            if (milton_state->view->scale == 1)
            {
                debug_scale_lock = true;
            }
        }
        else if (input->scale < 0 && milton_state->view->scale < view_scale_limit)
        {
            debug_scale_lock = false;
            milton_state->view->scale = (i32)(milton_state->view->scale * scale_factor) + 1;
        }
        milton_state->brush.radius = milton_state->brush_size * milton_state->view->scale;
        milton_state->eraser_brush.radius = milton_state->brush_size * milton_state->view->scale;
    }

    if (input->flags & MiltonInputFlags_SET_MODE_BRUSH)
    {
        milton_state->current_mode = MiltonMode_BRUSH;
    }
    if (input->flags & MiltonInputFlags_SET_MODE_ERASER)
    {
        milton_state->current_mode = MiltonMode_ERASER;
    }

    if (input->flags & MiltonInputFlags_UNDO)
    {
        if (milton_state->working_stroke.num_points == 0 && milton_state->num_strokes > 0)
        {
            milton_state->num_strokes--;
            milton_state->num_redos++;
        }
        else if (milton_state->working_stroke.num_points > 0)
        {
            // Commit working stroke.
            assert(!"NPE");
        }
    }
    else if (input->flags & MiltonInputFlags_REDO)
    {
        if (milton_state->num_redos > 0)
        {
            milton_state->num_strokes++;
            milton_state->num_redos--;
        }
    }

    if (input->flags & MiltonInputFlags_RESET)
    {
        render_flags |= MiltonRenderFlags_full_redraw;
        milton_state->num_strokes = 0;
        milton_state->strokes[0].num_points = 0;
        milton_state->working_stroke.num_points = 0;
    }
#if 0
    // ==== Rotate ======
    if (input->rotation != 0)
    {
        render_flags |= MiltonRenderFlags_full_redraw;
    }
    milton_state->view->rotation += input->rotation;
    while (milton_state->view->rotation < 0)
    {
        milton_state->view->rotation += 360;
    }
    while (milton_state->view->rotation >= 360)
    {
        milton_state->view->rotation -= 360;
    }
#endif

    b32 finish_stroke = false;
    if (input->point)
    {
        v2i point = *input->point;
        if (!is_user_drawing(milton_state) && is_inside_picker(&milton_state->picker, point))
        {
            ColorPickResult pick_result = picker_update(&milton_state->picker, point);
            if ((pick_result & ColorPickResult_change_color) &&
                (milton_state->current_mode == MiltonMode_BRUSH))
            {
                milton_state->brush.color = hsv_to_rgb(milton_state->picker.hsv);
            }
            milton_state->canvas_blocked = true;
            render_flags |= MiltonRenderFlags_picker_updated;
        }
        // Currently drawing
        else if (!milton_state->canvas_blocked)
        {
            if (milton_state->current_mode == MiltonMode_BRUSH)
            {
                milton_state->working_stroke.brush = milton_state->brush;
            }
            else if (milton_state->current_mode == MiltonMode_ERASER)
            {
                milton_state->working_stroke.brush = milton_state->eraser_brush;
            }
            v2i in_point = *input->point;

            if (milton_state->working_stroke.num_points == 0)
            {
                // Avoid creating really large update rects when starting new strokes
                milton_state->last_raster_input = in_point;
            }
            v2i canvas_point = raster_to_canvas(milton_state->view, in_point);

            // TODO: make deque!!
            if (milton_state->working_stroke.num_points < LIMIT_STROKE_POINTS)
            {
                // Add to current stroke.
                int index = milton_state->working_stroke.num_points++;
                milton_state->working_stroke.points[index] = canvas_point;
            }

            milton_state->last_raster_input = in_point;
        }
        if (milton_state->canvas_blocked)
        {
            v2f fpoint = v2i_to_v2f(point);
            ColorPicker* picker = &milton_state->picker;
            if  (picker_wheel_active(picker))
            {
                //if (picker_is_within_wheel(picker, fpoint))
                if (is_inside_triangle(fpoint, picker->a, picker->b, picker->c))
                {
                    picker_wheel_deactivate(picker);
                }
                else if (milton_state->current_mode == MiltonMode_BRUSH)
                {

                    picker_update_wheel(&milton_state->picker, fpoint);
                    milton_state->brush.color = hsv_to_rgb(milton_state->picker.hsv);
                }
                render_flags |= MiltonRenderFlags_picker_updated;
            }
        }

        // Clear redo stack
        milton_state->num_redos = 0;
    }

    if (input->flags & MiltonInputFlags_END_STROKE)
    {
        if (milton_state->canvas_blocked)
        {
            picker_wheel_deactivate(&milton_state->picker);
            milton_state->canvas_blocked = false;
        }
        else
        {
            if (milton_state->num_strokes < 4096)
            {
                // Copy current stroke.
                // Note: Caution when moving points out of a stack array. This is a shallow copy
                milton_state->strokes[milton_state->num_strokes++] = milton_state->working_stroke;
                // Clear working_stroke
                {
                    milton_state->working_stroke.num_points = 0;
                }
            }
        }
    }

    milton_render(milton_state, render_flags);
}
