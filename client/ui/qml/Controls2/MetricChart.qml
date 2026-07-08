import QtQuick
import Style 1.0

Canvas {
    id: chart

    property var dataModel: []      // QVariantList of numbers
    property real maxValue: 100     // Y-axis max; 0 = auto-scale
    property color lineColor: AmneziaStyle.color.goldenApricot
    property color fillColor: Qt.rgba(251/255, 178/255, 106/255, 0.15)
    property color gridColor: AmneziaStyle.color.slateGray
    property string unit: "%"
    property string title: ""

    height: 120
    antialiasing: true

    onDataModelChanged: requestPaint()
    onWidthChanged: requestPaint()

    onPaint: {
        var ctx = getContext("2d");
        ctx.reset();

        var w = width;
        var h = height;
        var data = dataModel;
        if (!data || data.length === 0) return;

        // Determine scale
        var yMax = maxValue;
        if (yMax <= 0) {
            yMax = 1;
            for (var k = 0; k < data.length; ++k) {
                if (data[k] > yMax) yMax = data[k];
            }
            yMax *= 1.2; // 20% headroom
        }

        var padding = 4;
        var plotW = w - padding * 2;
        var plotH = h - padding * 2 - 18; // reserve for title
        var topOffset = 18;

        // Title
        if (title.length > 0) {
            ctx.fillStyle = AmneziaStyle.color.paleGray;
            ctx.font = "11px sans-serif";
            ctx.fillText(title, padding, 13);
        }

        // Horizontal grid lines (25%, 50%, 75%)
        ctx.strokeStyle = gridColor;
        ctx.lineWidth = 0.5;
        ctx.setLineDash([2, 4]);
        for (var g = 1; g <= 3; ++g) {
            var gy = topOffset + padding + plotH * (1 - g / 4);
            ctx.beginPath();
            ctx.moveTo(padding, gy);
            ctx.lineTo(padding + plotW, gy);
            ctx.stroke();
        }
        ctx.setLineDash([]);

        // Data line + fill
        var step = data.length > 1 ? plotW / (data.length - 1) : plotW;

        ctx.beginPath();
        for (var i = 0; i < data.length; ++i) {
            var x = padding + i * step;
            var y = topOffset + padding + plotH * (1 - Math.min(data[i], yMax) / yMax);
            if (i === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }

        // Stroke line
        ctx.strokeStyle = lineColor;
        ctx.lineWidth = 1.5;
        ctx.stroke();

        // Fill under curve
        ctx.lineTo(padding + (data.length - 1) * step, topOffset + padding + plotH);
        ctx.lineTo(padding, topOffset + padding + plotH);
        ctx.closePath();
        ctx.fillStyle = fillColor;
        ctx.fill();

        // Current value label
        var lastVal = data[data.length - 1];
        var valStr = lastVal < 10 ? lastVal.toFixed(1) : Math.round(lastVal).toString();
        ctx.fillStyle = lineColor;
        ctx.font = "bold 11px sans-serif";
        ctx.textAlign = "right";
        ctx.fillText(valStr + unit, w - padding, 13);
        ctx.textAlign = "left";
    }
}
