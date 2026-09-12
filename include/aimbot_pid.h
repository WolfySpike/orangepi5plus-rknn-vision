#pragma once

class PIDController {
public:
    float prev_err_x = 0.0f;
    float prev_err_y = 0.0f;
    float rem_x = 0.0f;
    float rem_y = 0.0f;

    void compute(float err_x, float err_y, int& move_x, int& move_y);
    void reset();
};
