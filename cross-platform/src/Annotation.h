#pragma once

// Annotation.h — Core data model for one drawn shape + the tool enum.
//
// Source: Annotation.swift (Spec 1 §1, Spec 3 §2).
//
// COORDINATE SYSTEM: all coordinates are stored in VIEW POINTS with origin at
// TOP-LEFT, +Y DOWN. The original AppKit view was `isFlipped == true` to achieve
// this; Qt's QWidget/QPainter are already top-left/+Y-down, so stored coordinates
// map 1:1 to Qt with NO Y-flip. Do not introduce a flip in the model.
//
// Annotation is a value type (cheap copy) — the undo/redo stack relies on this.

#include <QColor>
#include <QPointF>
#include <QString>
#include <QVector>
#include <Qt>
#include <optional>

// Seven toolbar tools, declared in natural toolbar order.
enum class Tool {
    Select,      // cursor: select / move / resize selection (not a drawing tool)
    Arrow,       // line with arrowhead (start -> end)
    Line,        // straight line (start -> end)
    Rectangle,   // outline rectangle (start/end opposite corners)
    FilledRect,  // filled rectangle (redaction) (start/end opposite corners)
    Pen,         // freehand polyline (the ONLY tool that uses points[])
    Text         // text label (text-specific fields)
};

// Stable string key for persistence (settings keys depend on these literals).
// IMPORTANT: keep "filledRect" camelCase verbatim — it is the persistence contract.
inline const char* toolKey(Tool t) {
    switch (t) {
    case Tool::Select:     return "select";
    case Tool::Arrow:      return "arrow";
    case Tool::Line:       return "line";
    case Tool::Rectangle:  return "rectangle";
    case Tool::FilledRect: return "filledRect";
    case Tool::Pen:        return "pen";
    case Tool::Text:       return "text";
    }
    return "";
}

// Text alignment. Maps NSTextAlignment {left, center, right} to Qt alignment.
enum class TextAlign { Left, Center, Right };

inline Qt::Alignment toQtAlignment(TextAlign a) {
    switch (a) {
    case TextAlign::Left:   return Qt::AlignLeft;
    case TextAlign::Center: return Qt::AlignHCenter;
    case TextAlign::Right:  return Qt::AlignRight;
    }
    return Qt::AlignLeft;
}

// One drawn shape. Per-tool field usage:
//   arrow/line          : start, end (arrow also draws arrowhead at end)
//   rectangle/filledRect: start, end as opposite corners (normalize before use)
//   pen                 : points[] (ignore start/end for render)
//   text                : start = top-left anchor of text box; text/fontSize/
//                         rotation/bgColor/alignment apply
struct Annotation {
    Tool tool = Tool::Select;
    QColor color;                       // stroke/fill/text foreground (store full RGBA)
    qreal lineWidth = 3.0;              // stroke width in POINTS
    QPointF start;                      // start / first corner / text anchor
    QPointF end;                        // end / opposite corner / arrow tip
    QVector<QPointF> points;            // pen polyline only (includes start as first point)

    // Text-only fields (must be initialized for all tools):
    QString text;                       // text content; default ""
    qreal fontSize = 18.0;             // point size; default 18
    qreal rotation = 0.0;              // RADIANS (0 = upright). QPainter::rotate wants
                                        // degrees -> multiply by 180/M_PI when applying.
    std::optional<QColor> bgColor;      // text background; nullopt = NO background rect
                                        // at all (NOT transparent black).
    TextAlign alignment = TextAlign::Left;

    // Text styling (design "Text" panel): font family (empty = system default),
    // and bold / italic / underline. Applied via fontForAnnotation() everywhere.
    QString fontFamily;                 // "" = system default; else family name
    bool bold = false;
    bool italic = false;
    bool underline = false;
};
