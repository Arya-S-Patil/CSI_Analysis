function doPost(e) {
  var SPREADSHEET_ID = "13UEGaRlMoSX25IygE_VM0yKf8-OIMsYnpscMj_lCM8s";

  var sheet = SpreadsheetApp
    .openById(SPREADSHEET_ID)
    .getActiveSheet();

  var d        = JSON.parse(e.postData.contents);
  var apIndex  = d.ap_index;
  var samples  = d.samples;

  var NUM_SUBCARRIERS = 64;
  var NUM_PACKETS     = 50;

  // ── Column layout (1-indexed) ────────────────────────────────────────────
  //  1: Subcarrier
  //  2: Packet
  //  3: Pi5_AP1_real      7: Pi5_AP2_real
  //  4: Pi5_AP1_imag      8: Pi5_AP2_imag
  //  5: Pi5_AP1_rssi      9: Pi5_AP2_rssi
  //  6: Pi5_AP1_amp      10: Pi5_AP2_amp
  //  7: Pi5_AP1_angle    11: Pi5_AP2_angle  ← shifts AP2 block by 2
  // ────────────────────────────────────────────────────────────────────────
  var NUM_COLS = 12;  // 2 key cols + 5 data cols × 2 APs

  var colBase  = apIndex === 0 ? 3 : 8;  // first data col for this AP
  // offsets within the AP block: real=0, imag=1, rssi=2, amp=3, angle=4
  var COL_REAL  = colBase + 0;
  var COL_IMAG  = colBase + 1;
  var COL_RSSI  = colBase + 2;
  var COL_AMP   = colBase + 3;
  var COL_ANGLE = colBase + 4;

  // ── Initialize sheet header + skeleton if needed ─────────────────────────
  var headerCell = sheet.getRange(1, 1).getValue();
  if (headerCell !== "Subcarrier") {
    sheet.clearContents();

    sheet.getRange(1, 1, 1, NUM_COLS).setValues([[
      "Subcarrier", "Packet",
      "Pi5_AP1_real", "Pi5_AP1_imag", "Pi5_AP1_rssi", "Pi5_AP1_amp", "Pi5_AP1_angle_rad",
      "Pi5_AP2_real", "Pi5_AP2_imag", "Pi5_AP2_rssi", "Pi5_AP2_amp", "Pi5_AP2_angle_rad"
    ]]);

    var skeleton = [];
    for (var sc = 0; sc < NUM_SUBCARRIERS; sc++) {
      for (var pkt = 0; pkt < NUM_PACKETS; pkt++) {
        skeleton.push([sc, pkt, "", "", "", "", "", "", "", "", "", ""]);
      }
    }
    sheet.getRange(2, 1, skeleton.length, NUM_COLS).setValues(skeleton);
  }

  // ── Batch-write incoming samples ─────────────────────────────────────────
  // Collect all updates into a single setValues call per column group
  // instead of one setValue per cell — much faster for 50-row chunks.
  var updates = {};  // keyed by row number, value = partial column array

  samples.forEach(function(s) {
    var row = 2 + (s.subcarrier * NUM_PACKETS) + s.packet;
    updates[row] = {
      real:  s.real,
      imag:  s.imag,
      rssi:  s.rssi  !== undefined ? s.rssi  : "",
      amp:   s.amplitude !== undefined ? s.amplitude : "",
      angle: s.angle_rad !== undefined ? s.angle_rad : ""
    };
  });

  // Write each row individually (rows are non-contiguous across subcarriers
  // so a single block write isn't possible without re-reading the sheet).
  // Using getRange(row, colBase, 1, 5) writes all 5 AP columns in one call.
  Object.keys(updates).forEach(function(row) {
    var u = updates[row];
    sheet.getRange(Number(row), colBase, 1, 5).setValues([[
      u.real, u.imag, u.rssi, u.amp, u.angle
    ]]);
  });

  return ContentService
    .createTextOutput("OK")
    .setMimeType(ContentService.MimeType.TEXT);
}

// ── Test harness ─────────────────────────────────────────────────────────────
function testPost() {
  var testData = {
    ap_index: 0,
    samples: [
      { subcarrier: 0,  packet: 0,  real: 99,  imag: 88,  rssi: -55, amplitude: 132.45, angle_rad:  0.72734 },
      { subcarrier: 0,  packet: 1,  real: 77,  imag: 66,  rssi: -57, amplitude: 101.63, angle_rad:  0.70734 },
      { subcarrier: 1,  packet: 0,  real: 55,  imag: 44,  rssi: -60, amplitude:  70.43, angle_rad:  0.67474 },
      { subcarrier: 63, packet: 49, real: 11,  imag: 22,  rssi: -72, amplitude:  24.60, angle_rad:  1.10715 }
    ]
  };

  var e = { postData: { contents: JSON.stringify(testData) } };
  var result = doPost(e);
  Logger.log("Result: " + result.getContent());
}