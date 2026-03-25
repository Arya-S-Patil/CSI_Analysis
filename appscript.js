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
  var NUM_ROWS        = NUM_SUBCARRIERS * NUM_PACKETS;
  var NUM_COLS        = 12;

  // AP column start
  var colBase = apIndex === 0 ? 3 : 8;

  // ─────────────────────────────────────────────
  // ✅ Ensure correct sheet structure
  // ─────────────────────────────────────────────
  if (sheet.getLastColumn() !== NUM_COLS || sheet.getLastRow() === 0) {
    sheet.clear();

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

    sheet.getRange(2, 1, NUM_ROWS, NUM_COLS).setValues(skeleton);
  }

  // ─────────────────────────────────────────────
  // ✅ Read full sheet once
  // ─────────────────────────────────────────────
  var range = sheet.getRange(2, 1, NUM_ROWS, NUM_COLS);
  var data  = range.getValues();

  // ─────────────────────────────────────────────
  // ✅ Update values
  // ─────────────────────────────────────────────
  samples.forEach(function(s) {
    var rowIndex = (s.subcarrier * NUM_PACKETS) + s.packet;

    var colOffset = colBase - 1;

    data[rowIndex][colOffset + 0] = s.real;
    data[rowIndex][colOffset + 1] = s.imag;
    data[rowIndex][colOffset + 2] = (s.rssi !== undefined) ? s.rssi : "";
    data[rowIndex][colOffset + 3] = (s.amplitude !== undefined) ? s.amplitude : "";
    data[rowIndex][colOffset + 4] = (s.angle_rad !== undefined) ? s.angle_rad : "";
  });

  // ─────────────────────────────────────────────
  // ✅ Single write (critical)
  // ─────────────────────────────────────────────
  range.setValues(data);

  return ContentService
    .createTextOutput("OK")
    .setMimeType(ContentService.MimeType.TEXT);
}