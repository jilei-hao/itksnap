// Read the function library
include("Library");

// ---------------------------------------------------------------------------
// Workspace loading, mesh update and 4D replay while the reference space is a
// 4D segmentation on its own grid (seg_anchor). See
// test_SegAnchor4DSwitching.js for the data and the probe point P.
//
//   1. img4d_11f_seganchor.itksnap (main image, label1_x2crop, label2_x15)
//      opens with the FIRST segmentation active (cf65a583).
//   2. The mesh goes stale on a new time point or segmentation, and updates,
//      with either segmentation as the reference.
//   3. With continuous mesh update on, 4D replay advances the time point and
//      leaves the spatial cursor on P.
// ---------------------------------------------------------------------------

var P_LABEL1 = [7, 29, 21];
var P_LABEL2 = [29, 23, 20];

function validateCursorXYZ(p)
{
    engine.validateChildProperty(mainwin, "inCursorX_4D", "value", p[0]);
    engine.validateChildProperty(mainwin, "inCursorY_4D", "value", p[1]);
    engine.validateChildProperty(mainwin, "inCursorZ_4D", "value", p[2]);
}

// The mesh must be stale before the update, otherwise the click does nothing
// and updateMeshAndCheck() passes vacuously
function updateStaleMesh()
{
    engine.validateChildProperty(mainwin, "btnUpdateMesh", "enabled", true);
    updateMeshAndCheck();
}

//=== Case 1: the first segmentation, label1_x2crop, is active after loading.
//=== At P and time point 1 only label 1 is present; in label2_x15's grid the
//=== same voxel coordinates are elsewhere, unlabeled, with another intensity.
openWorkspace("img4d_11f_seganchor.itksnap");
setCursor4D(P_LABEL1[0], P_LABEL1[1], P_LABEL1[2], 1);
engine.validateChildProperty(mainwin, "outLabelText", "text", "Label 1");
engine.validateValue(readVoxelIntensity(0), 106.88, 0.1);

//=== Case 2: mesh with label1_x2crop as reference, at time points 1 and 4
updateStaleMesh();
setCursor4D(P_LABEL1[0], P_LABEL1[1], P_LABEL1[2], 4);
updateStaleMesh();

//=== '}': label2_x15 is the reference; mesh at time points 4 and 9
engine.trigger("actionActivateNextSegmentationLayer");
engine.sleep(500);
validateCursorXYZ(P_LABEL2);
updateStaleMesh();
setCursor4D(P_LABEL2[0], P_LABEL2[1], P_LABEL2[2], 9);
updateStaleMesh();

//=== Case 3: continuous update + replay, label2_x15 still the reference.
//=== The action is checkable and its state may come from preferences.
var actContinuous = engine.findChild(mainwin, "actionContinuous_Update");
if (!engine.getProperty(actContinuous, "checked"))
    engine.trigger("actionContinuous_Update");
engine.sleep(500);
engine.validateProperty(actContinuous, "checked", true);

setCursor4D(P_LABEL2[0], P_LABEL2[1], P_LABEL2[2], 1);
engine.sleep(2000);

engine.trigger("actionLayerInspector");
engine.sleep(1000);
var layerdialog = engine.findChild(mainwin, "dlgLayerInspector");
var row0 = engine.findChild(layerdialog, "wgtRowDelegate_0000");
engine.setProperty(row0, "selected", true);
engine.sleep(500);

var grp4D = engine.findChild(layerdialog, "grp4DProperties");
if (!engine.getProperty(grp4D, "visible"))
    engine.testFailed("4D Property Group not visible after selecting the main image row");

var btnReplay = engine.findChild(grp4D, "btn4DReplay");
engine.setChildProperty(grp4D, "in4DReplayInterval", "text", "200");

//=== Sample the time point during replay. Replay wraps around, so a single
//=== before/after comparison could see the same frame by chance.
var seen = {};
var nSeen = 0;
engine.click(btnReplay);   // start
for (var i = 0; i < 16; i++)
{
    engine.sleep(500);
    var tp = engine.getChildProperty(mainwin, "inCursorT_4D", "value");
    if (!seen[tp])
    {
        seen[tp] = true;
        nSeen++;
    }
}
engine.click(btnReplay);   // stop
engine.sleep(500);
engine.print("Replay visited " + nSeen + " distinct time points: " + Object.keys(seen));

if (nSeen < 3)
    engine.testFailed("4D replay did not advance with a non-anchored 4D segmentation as reference");

engine.invoke(layerdialog, "close");
validateCursorXYZ(P_LABEL2);
