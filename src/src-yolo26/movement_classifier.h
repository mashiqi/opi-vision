#pragma once

#include <string>

class MovementClassifier {
public:
    MovementClassifier() : state_("stationary"), high_hits_(0), low_since_(0) {}

    const std::string& update(float speed_diagonals_per_second, double now) {
        if (speed_diagonals_per_second >= 0.015f) {
            ++high_hits_;
            low_since_ = 0;
            if (high_hits_ >= 3) state_ = "moving";
        } else if (speed_diagonals_per_second < 0.010f) {
            high_hits_ = 0;
            if (!low_since_) low_since_ = now;
            if (now - low_since_ >= 2.0) state_ = "stationary";
        } else {
            high_hits_ = 0;
            low_since_ = 0;
        }
        return state_;
    }

private:
    std::string state_;
    unsigned int high_hits_;
    double low_since_;
};
