#include <ipc/mq.h>
#include <serial/serial.h>
#include <time/time.h>
#include <process/process.h>

#include <stella_user.h>

extern bool g_arrow_up_pressed;
extern bool g_arrow_down_pressed;

constexpr int WINDOW_WIDTH = 560;
constexpr int WINDOW_HEIGHT = 480;
constexpr int PADDLE_WIDTH = 10;
constexpr int PADDLE_HEIGHT = 80;
constexpr int BALL_SIZE = 10;
constexpr int INITIAL_PADDLE_SPEED = 6;
constexpr int INITIAL_BALL_SPEED_X = 5;
constexpr int INITIAL_BALL_SPEED_Y = 5;
constexpr int BALL_SPEED_INCREMENT = 1;  // Increase by 1 per hit

struct Paddle {
    int x, y;
    int speed;
};

struct Ball {
    int x, y;
    int dx, dy;
};

Paddle left_paddle = { 10, WINDOW_HEIGHT / 2 - PADDLE_HEIGHT / 2, INITIAL_PADDLE_SPEED };
Paddle right_paddle = { WINDOW_WIDTH - 20, WINDOW_HEIGHT / 2 - PADDLE_HEIGHT / 2, INITIAL_PADDLE_SPEED };
Ball ball = { WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, INITIAL_BALL_SPEED_X, INITIAL_BALL_SPEED_Y };
int left_score = 0, right_score = 0;

void update_paddles() {
    if (g_arrow_up_pressed && left_paddle.y > 0)
        left_paddle.y -= left_paddle.speed;
    if (g_arrow_down_pressed && left_paddle.y < WINDOW_HEIGHT - PADDLE_HEIGHT)
        left_paddle.y += left_paddle.speed;

    // AI movement prediction
    int predicted_ball_y = ball.y + (ball.dy * (right_paddle.x - ball.x) / (ball.dx != 0 ? ball.dx : 1));
    if (predicted_ball_y < right_paddle.y + PADDLE_HEIGHT / 2 && right_paddle.y > 0)
        right_paddle.y -= right_paddle.speed;
    if (predicted_ball_y > right_paddle.y + PADDLE_HEIGHT / 2 && right_paddle.y < WINDOW_HEIGHT - PADDLE_HEIGHT)
        right_paddle.y += right_paddle.speed;
}

void update_ball() {
    ball.x += ball.dx;
    ball.y += ball.dy;

    // Ball collision with top and bottom walls
    if (ball.y <= 0 || ball.y >= WINDOW_HEIGHT - BALL_SIZE)
        ball.dy = -ball.dy;

    // Ball collision with paddles
    if ((ball.x <= left_paddle.x + PADDLE_WIDTH && ball.y >= left_paddle.y && 
         ball.y <= left_paddle.y + PADDLE_HEIGHT) ||
        (ball.x + BALL_SIZE >= right_paddle.x && ball.y >= right_paddle.y && 
         ball.y <= right_paddle.y + PADDLE_HEIGHT)) {
        ball.dx = (ball.dx > 0 ? ball.dx + BALL_SPEED_INCREMENT : ball.dx - BALL_SPEED_INCREMENT);
        ball.dy = (ball.dy > 0 ? ball.dy + BALL_SPEED_INCREMENT : ball.dy - BALL_SPEED_INCREMENT);
        ball.dx = -ball.dx;
    }

    // Scoring conditions
    if (ball.x <= 0) { 
        right_score++; 
        ball = { WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, INITIAL_BALL_SPEED_X, INITIAL_BALL_SPEED_Y };
    }
    if (ball.x >= WINDOW_WIDTH) { 
        left_score++; 
        ball = { WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, -INITIAL_BALL_SPEED_X, INITIAL_BALL_SPEED_Y };
    }
    
    // Increase AI speed if player leads
    int score_diff = left_score - right_score;
    right_paddle.speed = INITIAL_PADDLE_SPEED + (score_diff > 0 ? score_diff : 0);
}

int main() {
    if (!stella_ui::connect_to_compositor()) {
        serial::printf("[PONG] Failed to connect to compositor\n");
        return -1;
    }

    serial::printf("[PONG] Connected to compositor!\n");

    if (!stella_ui::create_window(WINDOW_WIDTH, WINDOW_HEIGHT, "Pong")) {
        serial::printf("[SNAKE] Failed to create a window\n");
        return -1;
    }

    kstl::shared_ptr<stella_ui::canvas> canvas;
    if (!stella_ui::request_map_window_canvas(canvas)) {
        serial::printf("[PONG] Failed to map window canvas\n");
        return -1;
    }

    canvas->set_background_color(stella_ui::color::dark_gray.to_argb());
    
    char score_text[64];
    while (true) {
        update_paddles();
        update_ball();

        canvas->clear();
        
        // Draw score
        sprintf(score_text, 63, "Score: %u - %u", left_score, right_score);
        canvas->draw_string(WINDOW_WIDTH / 2 - 40, 10, score_text, stella_ui::color::white.to_argb());
        
        // Draw paddles
        canvas->fill_rect(left_paddle.x, left_paddle.y, PADDLE_WIDTH, PADDLE_HEIGHT, stella_ui::color::blue.to_argb());
        canvas->fill_rect(right_paddle.x, right_paddle.y, PADDLE_WIDTH, PADDLE_HEIGHT, stella_ui::color::red.to_argb());
        
        // Draw ball
        canvas->fill_rect(ball.x, ball.y, BALL_SIZE, BALL_SIZE, stella_ui::color::white.to_argb());

        sched::yield();
        msleep(24);
    }

    return 0;
}
