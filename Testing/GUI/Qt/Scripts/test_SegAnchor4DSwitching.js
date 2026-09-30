// Read the function library
include("Library");

// ---------------------------------------------------------------------------
// The reference space follows the active segmentation (seg_anchor), tested
// with 4D data: a 4D main image and two 4D segmentations, each on its own
// grid. Checks, at one probe point and every time point:
//   - the label under the cursor comes from the active segmentation's own
//     time point;
//   - the main image is sampled at the same physical point and time point,
//     whichever segmentation is the reference;
//   - loading or switching segmentations keeps the time point and moves the
//     cursor to the same physical point in the new reference grid.
//
// Data (made from seg4d_11f_label{1,2}.nii.gz by exact nearest-neighbour
// resampling; 1-based voxel counts):
//   img4d_11f.nii.gz                 main image, 38x35x38x11
//   seg4d_11f_label1_x2crop.nii.gz   label 1, cropped and 2x finer, 14x58x66x11
//   seg4d_11f_label2_x15.nii.gz      label 2, full extent, 1.5x finer, 57x53x57x11
//
// The probe point P is voxel (7,29,21) of label1_x2crop, voxel (29,23,20) of
// label2_x15 and voxel (20,16,14) of the main image. Q is voxel (-1,29,21) of
// label1_x2crop, just outside its box, which is where the cursor may now go
// (it stays inside the main image): voxel (23,23,20) of label2_x15 and
// (16,16,14) of the main image. No mapping between the two segmentation grids
// is a rounding tie, so the cursor positions below are exact.
// ---------------------------------------------------------------------------

var P_LABEL1 = [7, 29, 21];
var P_LABEL2 = [29, 23, 20];

// Values at P for time points 1..11, read from the image files
var LABEL1_AT_P = [1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0];
var LABEL2_AT_P = [0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 0];
var MAIN_AT_P   = [106.88, 160.93, 100.77, 115.71, 151.63, 144.12,
                   112.53, 59.86, 78.06, 71.13, 57.40];

var Q_LABEL1 = [-1, 29, 21];
var Q_LABEL2 = [23, 23, 20];

// Main image values at Q for time points 1..11; neither segmentation has a
// label there
var MAIN_AT_Q   = [106.55, 142.87, 163.18, 210.52, 208.14, 207.82,
                   189.36, 115.98, 134.37, 105.37, 78.91];

function labelName(value)
{
    return value == 0 ? "Clear Label" : "Label " + value;
}

function validateCursor(p, t)
{
    engine.validateChildProperty(mainwin, "inCursorX_4D", "value", p[0]);
    engine.validateChildProperty(mainwin, "inCursorY_4D", "value", p[1]);
    engine.validateChildProperty(mainwin, "inCursorZ_4D", "value", p[2]);
    engine.validateChildProperty(mainwin, "inCursorT_4D", "value", t);
}

// Visit every time point at p and check the label and the main image value
function validateAllTimePoints(p, labels)
{
    for (var t = 1; t <= 11; t++)
    {
        setCursor4D(p[0], p[1], p[2], t);
        engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(labels[t - 1]));
        engine.validateValue(readVoxelIntensity(0), MAIN_AT_P[t - 1], 0.1);
    }
}

// Selecting a segmentation row in the Layer Inspector makes it the active
// segmentation (ImageLayerTableRowModel::SetActivated)
function selectLayerRow(row)
{
    engine.trigger("actionLayerInspector");
    engine.sleep(500);
    var dlg = engine.findChild(mainwin, "dlgLayerInspector");
    engine.setProperty(engine.findChild(dlg, row), "selected", true);
    engine.sleep(500);
    engine.invoke(dlg, "close");
    engine.sleep(500);
}

// Note: this selects the row, so call it only for the active segmentation or
// the main image
function validateLayerGrid(row, spacing, dims)
{
    var info = getLayerResolutionInfo(row);
    engine.validateValue(info.spacingX, spacing[0], 0.002);
    engine.validateValue(info.spacingY, spacing[1], 0.002);
    engine.validateValue(info.spacingZ, spacing[2], 0.002);
    engine.validateValue(info.dimX, dims[0]);
    engine.validateValue(info.dimY, dims[1]);
    engine.validateValue(info.dimZ, dims[2]);
}

//=== 4D main image; label1_x2crop replaces the blank segmentation and becomes
//=== the reference space
openMainImage("img4d_11f.nii.gz");
openSegmentation("seg4d_11f_label1_x2crop.nii.gz");

engine.validateChildProperty(mainwin, "inCursorT_4D", "maximum", 11);
validateLayerGrid("wgtRowDelegate_0000", [5.566, 5.529, 4.845], ["38", "35", "38"]);
validateLayerGrid("wgtRowDelegate_0001", [2.783, 2.765, 2.422], ["14", "58", "66"]);

//=== Every time point, label1_x2crop active
validateAllTimePoints(P_LABEL1, LABEL1_AT_P);

//=== Load label2_x15 additively at time point 5; it becomes the reference.
//=== The cursor must land on P in the new grid, and the time point must stay 5
setCursor4D(P_LABEL1[0], P_LABEL1[1], P_LABEL1[2], 5);
openAdditionalSegmentation("seg4d_11f_label2_x15.nii.gz");
validateCursor(P_LABEL2, 5);
engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(LABEL2_AT_P[4]));
validateLayerGrid("wgtRowDelegate_0002", [3.710, 3.686, 3.230], ["57", "53", "57"]);

//=== Every time point, label2_x15 active: same main image values
validateAllTimePoints(P_LABEL2, LABEL2_AT_P);

//=== '{' back to label1_x2crop at time point 4 (label 1 absent, label 2
//=== present at P): time point kept, cursor back on P, label1's value shown
setCursor4D(P_LABEL2[0], P_LABEL2[1], P_LABEL2[2], 4);
engine.trigger("actionActivatePreviousSegmentationLayer");
engine.sleep(500);
validateCursor(P_LABEL1, 4);
engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(LABEL1_AT_P[3]));
engine.validateValue(readVoxelIntensity(0), MAIN_AT_P[3], 0.1);

//=== '}' forward to label2_x15 at time point 10
setCursor4D(P_LABEL1[0], P_LABEL1[1], P_LABEL1[2], 10);
engine.trigger("actionActivateNextSegmentationLayer");
engine.sleep(500);
validateCursor(P_LABEL2, 10);
engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(LABEL2_AT_P[9]));
engine.validateValue(readVoxelIntensity(0), MAIN_AT_P[9], 0.1);

//=== Selecting label1_x2crop's row in the Layer Inspector activates it too.
//=== At time point 8 only label 1 is present at P.
setCursor4D(P_LABEL2[0], P_LABEL2[1], P_LABEL2[2], 8);
engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(LABEL2_AT_P[7]));
selectLayerRow("wgtRowDelegate_0001");
validateCursor(P_LABEL1, 8);
engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(LABEL1_AT_P[7]));
engine.validateValue(readVoxelIntensity(0), MAIN_AT_P[7], 0.1);

//=== The grid survived the round trip
validateLayerGrid("wgtRowDelegate_0001", [2.783, 2.765, 2.422], ["14", "58", "66"]);

//=== Q, outside label1_x2crop's box: no label, the main image is still sampled
var tq = [2, 4, 9];
for (var i = 0; i < tq.length; i++)
{
    setCursor4D(Q_LABEL1[0], Q_LABEL1[1], Q_LABEL1[2], tq[i]);
    engine.validateChildProperty(mainwin, "outLabelText", "text", "Clear Label");
    engine.validateValue(readVoxelIntensity(0), MAIN_AT_Q[tq[i] - 1], 0.1);
}

//=== Switching with the cursor at Q keeps it at Q, in both directions, even
//=== though Q is outside the box of the segmentation switched back to
engine.trigger("actionActivateNextSegmentationLayer");
engine.sleep(500);
validateCursor(Q_LABEL2, 9);
engine.validateValue(readVoxelIntensity(0), MAIN_AT_Q[8], 0.1);

engine.trigger("actionActivatePreviousSegmentationLayer");
engine.sleep(500);
validateCursor(Q_LABEL1, 9);
engine.validateValue(readVoxelIntensity(0), MAIN_AT_Q[8], 0.1);
