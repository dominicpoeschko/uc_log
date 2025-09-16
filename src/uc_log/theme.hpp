#pragma once

#include <ftxui/screen/color.hpp>

namespace uc_log {

struct Theme {
    struct Text {
        static inline ftxui::Color const timestamp    = ftxui::Color::Cyan;
        static inline ftxui::Color const ucTime       = ftxui::Color::Magenta;
        static inline ftxui::Color const functionName = ftxui::Color::Blue;
        static inline ftxui::Color const normal       = ftxui::Color::Default;
        static inline ftxui::Color const separator    = ftxui::Color::GrayDark;
        static inline ftxui::Color const metadata     = ftxui::Color::GrayDark;
    };

    struct Status {
        static inline ftxui::Color const success  = ftxui::Color::Green;
        static inline ftxui::Color const error    = ftxui::Color::Red;
        static inline ftxui::Color const warning  = ftxui::Color::Yellow;
        static inline ftxui::Color const info     = ftxui::Color::Cyan;
        static inline ftxui::Color const inactive = ftxui::Color::GrayDark;
        static inline ftxui::Color const active   = ftxui::Color::Green;
        static inline ftxui::Color const running  = ftxui::Color::Yellow;
        static inline ftxui::Color const failed   = ftxui::Color::Red;
    };

    struct Message {
        static inline ftxui::Color const fatal  = ftxui::Color::Red;
        static inline ftxui::Color const error  = ftxui::Color::Magenta;
        static inline ftxui::Color const status = ftxui::Color::Green;
    };

    struct Button {
        struct Background {
            static inline ftxui::Color const destructive = ftxui::Color::RedLight;
            static inline ftxui::Color const positive    = ftxui::Color::GreenLight;
            static inline ftxui::Color const reset       = ftxui::Color::CyanLight;
            static inline ftxui::Color const build       = ftxui::Color::YellowLight;
            static inline ftxui::Color const debug       = ftxui::Color::MagentaLight;
            static inline ftxui::Color const settings    = ftxui::Color::BlueLight;
            static inline ftxui::Color const danger      = ftxui::Color::Red;
        };

        static inline ftxui::Color const text       = ftxui::Color::Black;
        static inline ftxui::Color const textOnDark = ftxui::Color::Black;
    };

    struct Header {
        static inline ftxui::Color const primary   = ftxui::Color::Cyan;
        static inline ftxui::Color const secondary = ftxui::Color::Green;
        static inline ftxui::Color const accent    = ftxui::Color::Magenta;
        static inline ftxui::Color const warning   = ftxui::Color::Yellow;
    };

    struct Data {
        static inline ftxui::Color const scope = ftxui::Color::Cyan;
        static inline ftxui::Color const name  = ftxui::Color::Green;
        static inline ftxui::Color const value = ftxui::Color::Yellow;
        static inline ftxui::Color const unit  = ftxui::Color::Magenta;
        static inline ftxui::Color const count = ftxui::Color::GrayDark;
        static inline ftxui::Color const icon  = ftxui::Color::Yellow;
    };

    struct UI {
        static inline ftxui::Color const separator  = ftxui::Color::GrayDark;
        static inline ftxui::Color const border     = ftxui::Color::Default;
        static inline ftxui::Color const background = ftxui::Color::Default;
        static inline ftxui::Color const remove     = ftxui::Color::Red;
        static inline ftxui::Color const add        = ftxui::Color::Green;
    };
};

}   // namespace uc_log
