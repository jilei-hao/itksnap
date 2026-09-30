// Read the function library
include("Library");

// ---------------------------------------------------------------------------
// Loading a 3D segmentation into a 4D workspace, when the segmentations are on
// their own grids (seg_anchor). A 3D file does not add a layer: it replaces the
// current time point of the selected segmentation.
//
//   1. 3D file on a different grid from the selected 4D segmentation: the load
//      is refused and the segmentation is left untouched.
//   2. 3D file on the same grid: only the current time point changes.
//
// Data: seg3d_11f_label1_tp4_x2crop.nii.gz is time point 5 (1-based) of
// seg4d_11f_label1_x2crop.nii.gz. See test_SegAnchor4DSwitching.js for the
// other files and the probe point P.
// ---------------------------------------------------------------------------

var P_LABEL1 = [7, 29, 21];
var P_LABEL2 = [29, 23, 20];

// Values at P for time points 1..11, read from the image files
var LABEL1_AT_P = [1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0];
var LABEL2_AT_P = [0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 0];

function labelName(value)
{
    return value == 0 ? "Clear Label" : "Label " + value;
}

function validateAllTimePoints(p, labels)
{
    for (var t = 1; t <= 11; t++)
    {
        setCursor4D(p[0], p[1], p[2], t);
        engine.validateChildProperty(mainwin, "outLabelText", "text", labelName(labels[t - 1]));
    }
}

openMainImage("img4d_11f.nii.gz");

//=== Case 1: different grid. label2_x15 is the only segmentation; the 3D file
//=== is on the label1_x2crop grid and must be refused.
openSegmentation("seg4d_11f_label2_x15.nii.gz");
setCursor4D(P_LABEL2[0], P_LABEL2[1], P_LABEL2[2], 4);
engine.validateChildProperty(mainwin, "outLabelText", "text", "Label 2");

openSegmentation("seg3d_11f_label1_tp4_x2crop.nii.gz");

//=== The wizard stays open with an error; close it
var wizard = engine.findChild(mainwin, "wizImageIO");
if (!wizard || !engine.getProperty(wizard, "visible"))
    engine.testFailed("3D segmentation on a different grid was not refused");
engine.invoke(wizard, "reject");
engine.sleep(500);

//=== label2_x15 is unchanged at every time point
validateAllTimePoints(P_LABEL2, LABEL2_AT_P);

//=== Case 2: same grid. label1_x2crop replaces label2_x15. Time point 7 has
//=== no label at P; the 3D file has.
openSegmentation("seg4d_11f_label1_x2crop.nii.gz");
setCursor4D(P_LABEL1[0], P_LABEL1[1], P_LABEL1[2], 7);
engine.validateChildProperty(mainwin, "outLabelText", "text", "Clear Label");

openSegmentation("seg3d_11f_label1_tp4_x2crop.nii.gz");

//=== Only time point 7 changed
var expected = LABEL1_AT_P.slice();
expected[6] = LABEL1_AT_P[4];
validateAllTimePoints(P_LABEL1, expected);

//=== Still a single segmentation layer, on its own grid
var info = getLayerResolutionInfo("wgtRowDelegate_0001");
engine.validateValue(info.dimX, "14");
engine.validateValue(info.dimY, "58");
engine.validateValue(info.dimZ, "66");

engine.trigger("actionLayerInspector");
engine.sleep(500);
var dlg = engine.findChild(mainwin, "dlgLayerInspector");
if (engine.findChild(dlg, "wgtRowDelegate_0002"))
    engine.testFailed("A 3D segmentation loaded into a 4D workspace added a layer");
engine.invoke(dlg, "close");
