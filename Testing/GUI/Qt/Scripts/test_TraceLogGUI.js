// Read the function library
include("Library");

// Records a trace log through the GUI. ITK-SNAP is started with --trace-log
// (see CMakeLists.txt). The test changes slices with the keyboard and the
// slider, zooms and pans with the mouse, paints two strokes, undoes one and
// saves the segmentation. It then replays the saved log into a new, blank
// segmentation and checks the result is the same. TraceLogGUICheck then reads
// the log and checks that all of it was recorded with the view it happened in,
// and that replaying the log rebuilds the saved segmentation.

//=== Recording from the start shows in the window title
engine.sleep(1000);
var title = String(engine.getProperty(mainwin, "windowTitle"));
engine.print("Title: " + title);
engine.validateValue(title.indexOf("[Recording trace]") >= 0, true);

openMainImage("MRIcrop-orig.gipl.gz");

//=== Show just the axial view
engine.clickChild(mainwin, "btnAxial");
engine.sleep(500);
var panel0 = engine.findChild(mainwin, "panel0");
var canvas = engine.findChild(panel0, "sliceViewCanvas");

// Drag from (x0, y0) to (x1, y1) with a button held, in a few steps
function drag(button, x0, y0, x1, y1)
{
    engine.postMouseEvent(canvas, x0, y0, "press", button);
    engine.sleep(300);
    for (var i = 1; i <= 4; i++)
    {
        engine.postMouseEvent(canvas, x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4,
                              "move", button);
        engine.sleep(200);
    }
    engine.postMouseEvent(canvas, x1, y1, "release", button);
    engine.sleep(800);
}

//=== Crosshairs mode: change slices with the keyboard, zoom (right drag)
//=== and pan (middle drag)
engine.trigger("actionCrosshair");
engine.sleep(500);
engine.postKeyEvent(canvas, "PgUp");
engine.sleep(300);
engine.postKeyEvent(canvas, "PgUp");
engine.sleep(300);
engine.postKeyEvent(canvas, "PgDown");
engine.sleep(500);

//=== Move the slice slider next to the axial view
var slice_now = engine.getChildProperty(panel0, "inSlicePosition", "value");
engine.setChildProperty(panel0, "inSlicePosition", "value", slice_now - 4);
engine.sleep(500);
drag("right", 0.5, 0.5, 0.5, 0.3);
drag("middle", 0.5, 0.5, 0.6, 0.6);

//=== Paintbrush: two strokes, then undo the second
engine.trigger("actionPaintbrush");
engine.sleep(500);
var brushpanel = engine.findChild(mainwin, "pagePaintbrushTool");
engine.clickChild(brushpanel, "btnRound");
engine.setChildProperty(brushpanel, "inBrushSizeSpinbox", "value", 5);
engine.sleep(500);
drag("left", 0.4, 0.5, 0.6, 0.5);
drag("left", 0.4, 0.6, 0.6, 0.6);
engine.trigger("actionUndo");
engine.sleep(500);

// Number of voxels with the given label in the active segmentation
function countVoxels(label)
{
    engine.trigger("actionVolumesAndStatistics");
    engine.sleep(1000);
    var dlg = engine.findWidget("dlgStatistics");
    engine.clickChild(dlg, "btnUpdate");
    engine.sleep(1000);
    var table = engine.findChild(dlg, "tvVolumes");
    var row = engine.findItemRow(table, label);
    var count = (row === undefined || row === null) ? 0
                : parseInt(String(engine.tableItemText(table, row, 1)).replace(/,/g, ""));
    engine.invoke(dlg, "close");
    engine.sleep(500);
    return count;
}

var n_painted = countVoxels("Label 1");
engine.print("Painted voxels: " + n_painted);
engine.validateValue(n_painted > 0, true);

//=== Save the segmentation: the log is copied next to it
engine.trigger("actionSaveSegmentation");
engine.sleep(2000);
var dialog = engine.findChild(mainwin, "wizImageIO");
var out_file = tempdir + "/TraceLogGUI_seg_" + Date.now() + ".nii.gz";
engine.setChildProperty(dialog, "inFilename", "text", out_file);
engine.sleep(500);
engine.clickChild(dialog, "qt_wizard_finish");
engine.sleep(2000);
engine.print("Saved " + out_file);

//=== Replay the saved log into a new, blank segmentation: same result
engine.trigger("actionAddSegmentation_New");
engine.sleep(1000);
engine.validateValue(countVoxels("Label 1"), 0);
engine.callMethod(mainwin, "ReplayTraceLogFile", [out_file + ".trace.jsonl", -1]);
engine.sleep(2000);
var result = String(engine.getProperty(mainwin, "traceLogReplayResult"));
engine.print("Replay: " + result);
engine.validateValue(result.indexOf("identical") >= 0, true);
engine.validateValue(countVoxels("Label 1"), n_painted);
