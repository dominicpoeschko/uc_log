#pragma once

#include <ftxui/screen/color.hpp>

namespace uc_log {

struct Theme {
    struct Text {
        static constexpr ftxui::Color timestamp() { return ftxui::Color::Cyan; }

        static constexpr ftxui::Color ucTime() { return ftxui::Color::Magenta; }

        static constexpr ftxui::Color functionName() { return ftxui::Color::Blue; }

        static constexpr ftxui::Color normal() { return ftxui::Color::Default; }

        static constexpr ftxui::Color separator() { return ftxui::Color::GrayDark; }

        static constexpr ftxui::Color metadata() { return ftxui::Color::GrayDark; }
    };

    struct Status {
        static constexpr ftxui::Color success() { return ftxui::Color::Green; }

        static constexpr ftxui::Color error() { return ftxui::Color::Red; }

        static constexpr ftxui::Color warning() { return ftxui::Color::Yellow; }

        static constexpr ftxui::Color info() { return ftxui::Color::Cyan; }

        static constexpr ftxui::Color inactive() { return ftxui::Color::GrayDark; }

        static constexpr ftxui::Color active() { return ftxui::Color::Green; }

        static constexpr ftxui::Color running() { return ftxui::Color::Yellow; }

        static constexpr ftxui::Color failed() { return ftxui::Color::Red; }
    };

    struct Message {
        static constexpr ftxui::Color fatal() { return ftxui::Color::Red; }

        static constexpr ftxui::Color error() { return ftxui::Color::Magenta; }

        static constexpr ftxui::Color status() { return ftxui::Color::Green; }
    };

    struct Button {
        struct Background {
            static constexpr ftxui::Color destructive() { return ftxui::Color::RedLight; }

            static constexpr ftxui::Color positive() { return ftxui::Color::GreenLight; }

            static constexpr ftxui::Color reset() { return ftxui::Color::CyanLight; }

            static constexpr ftxui::Color build() { return ftxui::Color::YellowLight; }

            static constexpr ftxui::Color debug() { return ftxui::Color::MagentaLight; }

            static constexpr ftxui::Color settings() { return ftxui::Color::BlueLight; }

            static constexpr ftxui::Color danger() { return ftxui::Color::Red; }
        };

        static constexpr ftxui::Color text() { return ftxui::Color::Black; }

        static constexpr ftxui::Color textOnDark() { return ftxui::Color::Black; }
    };

    struct Header {
        static constexpr ftxui::Color primary() { return ftxui::Color::Cyan; }

        static constexpr ftxui::Color secondary() { return ftxui::Color::Green; }

        static constexpr ftxui::Color accent() { return ftxui::Color::Magenta; }

        static constexpr ftxui::Color warning() { return ftxui::Color::Yellow; }
    };

    struct Data {
        static constexpr ftxui::Color scope() { return ftxui::Color::Cyan; }

        static constexpr ftxui::Color name() { return ftxui::Color::Green; }

        static constexpr ftxui::Color value() { return ftxui::Color::Yellow; }

        static constexpr ftxui::Color unit() { return ftxui::Color::Magenta; }

        static constexpr ftxui::Color count() { return ftxui::Color::GrayDark; }

        static constexpr ftxui::Color icon() { return ftxui::Color::Yellow; }
    };

    struct UI {
        static constexpr ftxui::Color separator() { return ftxui::Color::GrayDark; }

        static constexpr ftxui::Color border() { return ftxui::Color::Default; }

        static constexpr ftxui::Color background() { return ftxui::Color::Default; }

        static constexpr ftxui::Color remove() { return ftxui::Color::Red; }

        static constexpr ftxui::Color add() { return ftxui::Color::Green; }
    };
};

}   // namespace uc_log
