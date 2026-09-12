#include "aimbot_labels.h"

const char* LABELS[] = {
    "Enemy_Body",
    "Enemy_Head",
    "Thermal",
    "Teammate",
    "Bot",
    "Knocked",
    "Dummy_Body",
    "Dummy_Head",
    "Negative",
};

const int LABELS_COUNT = sizeof(LABELS) / sizeof(LABELS[0]);

std::string get_label_name(int cls) {
    if (cls >= 0 && cls < LABELS_COUNT) {
        return std::string(LABELS[cls]);
    }
    return "Class_" + std::to_string(cls);
}
