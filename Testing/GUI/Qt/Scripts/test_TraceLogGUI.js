// Read the function library
include("Library");

// Records a trace log through the GUI. ITK-SNAP is started with --trace-log
// (see CMakeLists.txt), and starts from a segmentation with labels. The test
// changes slices with the keyboard and the
// slider, zooms and pans with the mouse, paints two strokes, undoes one and
// saves the segmentation. It then replays the log saved with it, which builds
// a new layer from the segmentation the session started from, and checks the
// result is the same. TraceLogGUICheck then reads
// the log and checks that all of it was recorded with the view it happened in,
// and that replaying the log rebuilds the saved segmentation.

//=== Recording from the start shows in the window title
engine.sleep(1000);
var title = String(engine.getProperty(mainwin, "windowTitle"));
engine.print("Title: " + title);
engine.validateValue(title.indexOf("[Recording trace]") >= 0, true);

openMainImage("MRIcrop-orig.gipl.gz");

//=== Start from a segmentation with labels: the log keeps a copy of it
openSegmentation("MRIcrop-seg.gipl.gz");

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

//=== Save the segmentation under a new name (Save As, so that the test data
//=== it was loaded from is never overwritten): the log is copied next to it
// Wait for each step of the wizard rather than a fixed time: Finish is
// enabled only once the wizard has checked the file name, and a click before
// that is ignored, leaving the segmentation unsaved
function waitFor(what, test)
{
    for (var i = 0; i < 40 && !test(); i++)
        engine.sleep(250);
    engine.print("Waited for " + what + ": " + test());
    engine.validateValue(test(), true);
}

engine.trigger("actionSaveSegmentationAs");
var dialog = null;
waitFor("the save wizard", function() {
    dialog = engine.findVisibleChild(mainwin, "wizImageIO");
    return dialog !== null && dialog !== undefined;
});
var out_file = tempdir + "/TraceLogGUI_seg_" + Date.now() + ".nii.gz";
engine.setChildProperty(dialog, "inFilename", "text", out_file);
waitFor("Finish", function() {
    return engine.getChildProperty(dialog, "qt_wizard_finish", "enabled") == true;
});
engine.clickChild(dialog, "qt_wizard_finish");
waitFor("the wizard to close", function() {
    var d = engine.findVisibleChild(mainwin, "wizImageIO");
    return d === null || d === undefined;
});
engine.sleep(1000);
engine.print("Saved " + out_file);

//=== Replay the log saved with the open segmentation, as Tools > Trace Log >
//=== Replay offers it: it is rebuilt in a new layer, the same as saved
engine.callMethod(mainwin, "ReplayTraceLogFile", ["", -1]);
engine.sleep(3000);
var result = String(engine.getProperty(mainwin, "traceLogReplayResult"));
engine.print("Replay: " + result);
engine.validateValue(result.indexOf("identical") >= 0, true);
engine.validateValue(result.indexOf("new segmentation layer") >= 0, true);
// The statistics table is occasionally read before it has caught up with the
// new layer, so read it again before calling a difference a failure
var n_replayed = countVoxels("Label 1");
for (var attempt = 2; attempt <= 3 && n_replayed != n_painted; attempt++)
{
    engine.print("Voxels in the replayed layer: " + n_replayed + "; reading again");
    engine.sleep(1000);
    n_replayed = countVoxels("Label 1");
}
engine.validateValue(n_replayed, n_painted);
