/* test_dvd_internal.h — internal struct definitions for DVD testing */
#ifndef TEST_DVD_INTERNAL_H
#define TEST_DVD_INTERNAL_H

/* Expose DVD game struct for testing */
struct wb_dvd_game {
    struct wb_dvd_project *proj;
    char name[256];
    int points_correct;
    int points_wrong;
    int question_count;
    int questions[256];
    int correct_buttons[256];
    int num_buttons[256];
    int easter_egg_count;
    int easter_egg_combos[32][8];
    int easter_egg_lengths[32];
    int easter_egg_targets[32];
    int branching_count;
    int branch_thresholds[32];
    int branch_targets[32];
    int timer_seconds;
    int timer_timeout_pgcn;
    int hidden_button_count;
    struct { float x, y, w, h; int target_title; int target_chapter; int up, down, left, right; } hidden_buttons[32];
    int hidden_targets[32];
    int parental_level;
    int parental_password;
};

/* Expose DVD project struct for testing */
struct wb_dvd_project {
    int format;
    int video_std;
    int aspect;
    struct { char video_path[512]; char audio_path[512]; double duration_sec; int title_idx; } titles[99];
    int title_count;
    char menu_bg_path[512];
    char menu_video_path[512];
    struct { float x, y, w, h; int target_title; int target_chapter; int up, down, left, right; } buttons[36];
    int button_count;
    int chapters[99][99];
    int chapter_count[99];
    char output_dir[512];
    int error;
    char error_msg[256];
};

#endif /* TEST_DVD_INTERNAL_H */
