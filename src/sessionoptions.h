#pragma once
// Command-line options for one session: validated in main.cpp, applied by
// MainWindow, never saved.
#include <QString>

#include <optional>

struct SessionOptions {
    std::optional<int> volume;               // 0-100
    std::optional<QString> theme;            // as given; resolved against custom themes later
    std::optional<QString> interpolation;    // off|linear|cubic|sinc
    std::optional<QString> backend;          // auto|null
    std::optional<double> speed;             // tempo factor, 0.05-20
};
