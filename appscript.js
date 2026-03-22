function doPost(e) {
  var SPREADSHEET_ID = "13UEGaRlMoSX25IygE_VM0yKf8-OIMsYnpscMj_lCM8s";

  var sheet = SpreadsheetApp
    .openById(SPREADSHEET_ID)
    .getActiveSheet();

  var d = JSON.parse(e.postData.contents);
  var apIndex = d.ap_index;
  var samples  = d.samples;

  var NUM_SUBCARRIERS = 64;
  var NUM_PACKETS     = 50;

  var realCol = apIndex === 0 ? 3 : 5;
  var imagCol = apIndex === 0 ? 4 : 6;

  // Initialize sheet if header is missing
  var headerCell = sheet.getRange(1, 1).getValue();
  if (headerCell !== "Subcarrier") {
    sheet.clearContents();

    sheet.getRange(1, 1, 1, 6).setValues([[
      "Subcarrier", "Packet",
      "Pi5_AP1_real", "Pi5_AP1_imag",
      "Pi5_AP2_real", "Pi5_AP2_imag"
    ]]);

    var skeleton = [];
    for (var sc = 0; sc < NUM_SUBCARRIERS; sc++) {
      for (var pkt = 0; pkt < NUM_PACKETS; pkt++) {
        skeleton.push([sc, pkt, "", "", "", ""]);
      }
    }
    sheet.getRange(2, 1, skeleton.length, 6).setValues(skeleton);
  }

  // Write incoming samples into the correct columns
  samples.forEach(function(s) {
    var row = 2 + (s.subcarrier * NUM_PACKETS) + s.packet;
    sheet.getRange(row, realCol).setValue(s.real);
    sheet.getRange(row, imagCol).setValue(s.imag);
  });

  return ContentService
    .createTextOutput("OK")
    .setMimeType(ContentService.MimeType.TEXT);
}

function testPost() {
  var testData = {
    ap_index: 0,
    samples: [
      {subcarrier: 0, packet: 0, real: 99, imag: 88},
      {subcarrier: 0, packet: 1, real: 77, imag: 66},
      {subcarrier: 1, packet: 0, real: 55, imag: 44},
      {subcarrier: 63, packet: 49, real: 11, imag: 22}
    ]
  };

  var e = { postData: { contents: JSON.stringify(testData) } };
  var result = doPost(e);
  Logger.log("Result: " + result.getContent());
}