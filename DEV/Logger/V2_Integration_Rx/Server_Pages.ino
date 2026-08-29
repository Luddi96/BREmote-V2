const char CONF_HTML[] PROGMEM = R"de4f(
<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Rx_V2 Configuration</title>
<style>
  body { font-family: Arial, sans-serif; margin: 20px; background-color: #cccccc; }
  table { width: 100%; border-collapse: collapse; margin-top: 20px; }
  th, td { padding: 8px; border: 1px solid #ddd; text-align: left; }
  input { padding: 6px; width: 160px; margin: 0; }
  button { padding: 10px 16px; margin-top: 20px; margin-right: 10px; font-size: 15px; cursor: pointer; }
  .variable-name { display: flex; align-items: center; }
  .tooltip { position: relative; display: inline-block; }
  .lightbulb {
    display: inline-block; width: 14px; height: 14px; background-color: #ffd700;
    border-radius: 50%; margin-right: 8px; cursor: help; vertical-align: middle;
  }
  .tooltip .tooltiptext {
    visibility: hidden; width: 260px; background-color: #333; color: #fff;
    text-align: left; border-radius: 6px; padding: 8px; position: absolute;
    z-index: 1; top: -5px; left: 125%; opacity: 0; transition: opacity 0.3s;
    font-size: 12px; box-shadow: 0px 2px 8px rgba(0,0,0,0.2);
  }
  .tooltip:hover .tooltiptext { visibility: visible; opacity: 1; }
  #status { margin-top: 15px; font-weight: bold; }
  .modal-overlay {
    display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%;
    background: rgba(0,0,0,0.5); align-items: center; justify-content: center; z-index: 10;
  }
  .modal-box {
    background: #fff; padding: 25px; border-radius: 8px; max-width: 380px; text-align: center;
  }
  .modal-box button { margin: 10px 8px 0 0; }
</style>
</head>
<body>

<h1>Rx_V2 Configuration</h1>
<div id="status">Loading configuration from device...</div>

<table>
  <thead>
    <tr><th>Setting</th><th>Value</th></tr>
  </thead>
  <tbody id="table-body"></tbody>
</table>

<button onclick="saveConfig()">Save</button>

<div class="modal-overlay" id="confirm-modal">
  <div class="modal-box">
    <p>Configuration saved to device.<br>Apply now or reboot to activate it?</p>
    <button onclick="applyConfig()">Apply now</button>
    <button onclick="rebootConfig()">Reboot device</button>
    <button onclick="closeModal()">Cancel</button>
  </div>
</div>

<script>
const cstruct = `
struct confStruct {
    //Version
    uint16_t version;
    
    uint16_t radio_preset; //1: 868MHz (EU), 2: 915MHz (US/AU)
    int16_t rf_power; //Tx power from -9 to 22

    uint16_t steering_type; //0: single motor, 1: diff motor, 2: servo
    uint16_t steering_influence; //How much (percentually) the steering influences the motor speeds
    uint16_t steering_inverted; //If steering is inverted or not
    int16_t trim; //Trim the steering

    //PWM min and max
    uint16_t PWM0_min;
    uint16_t PWM0_max;
    uint16_t PWM1_min;
    uint16_t PWM1_max;

    uint16_t failsafe_time; //Time after last packet until failsafe

    //Foil battery voltage settings
    uint16_t foil_num_cells; //Amount of cells in series e.g. 14 for a "14SxP" pack

    //Sensors
    uint16_t bms_det_active;
    uint16_t wet_det_active;

    uint16_t dummy_delete_me;

    //UART config
    uint16_t data_src; //0: off, 1:analog, 2: VESC UART

    // GPS features related flags
    uint16_t gps_en;         // GPS runtime enable flag (0=disabled, 1=enabled)
    uint16_t followme_mode;  // Follow-me runtime mode flag (0=disabled, 1=behind, 2=near_right, 3=near_left)
    uint16_t kalman_en;      // Kalman filter runtime enable flag (0=disabled, 1=enabled)

    //Follow-me
    float boogie_vmax_in_followme_kmh; // Maximum boogie speed in follow-me mode (km/h)
    float min_dist_m; // minimum allowed distance to the foiler
    float followme_smoothing_band_m; // smoothing band above min distance
    float foiler_low_speed_kmh; // low-speed threshold for safety stop (hysteresis)
    float zone_angle_enter_deg; // Half-angle for zone entry (deg)
    float zone_angle_exit_deg;  // Half-angle for zone exit (deg)
    float near_diag_offset_deg; // Offset from behind for NEAR modes (deg)
    
    //System parameters
    float ubat_cal; //ADC to volt cal for bat meas
    float ubat_offset; //Offset to add to analog/vesc measurement

    uint16_t tx_gps_stale_timeout_ms; // TX GPS data stale timeout (ms)

    //Logger
    uint16_t logger_en; // BREmote Logger runtime enable flag (0=disabled, 1=enabled)

    //Comms
    uint16_t paired;
    uint8_t own_address[3];
    uint8_t dest_address[3];
};
`;

const typeSizes = { 'uint8_t': 1, 'int8_t': 1, 'uint16_t': 2, 'int16_t': 2, 'uint32_t': 4, 'float': 4, 'double': 8 };

let globalCstructVars = [];
let globalBase64Array = null;
let originalBase64Input = '';
let originalFloatValues = {};

function parseCStruct(cstruct) {
  const variables = [];
  const lines = cstruct.split('\n');
  const varPattern = /(\w+)\s+(\w+)(\[(\d+)\])?\s*;\s*(?:\/\/\s*(.*))?/;
  for (let line of lines) {
    line = line.trim();
    if (!line || line.startsWith('//') || line.startsWith('struct') || line.startsWith('}')) continue;
    const match = line.match(varPattern);
    if (match) {
      const type = match[1];
      const varName = match[2];
      const arraySize = match[4] ? parseInt(match[4], 10) : null;
      const comment = match[5] ? match[5].trim() : null;
      const varSize = (typeSizes[type] || 0) * (arraySize || 1);
      if (varSize > 0) {
        variables.push({ name: varName, size: varSize, type: type, arraySize: arraySize, comment: comment });
      }
    }
  }
  return variables;
}

function base64ToArrayBuffer(base64) {
  const binaryString = atob(base64);
  const length = binaryString.length;
  const arrayBuffer = new Uint8Array(length);
  for (let i = 0; i < length; i++) arrayBuffer[i] = binaryString.charCodeAt(i);
  return arrayBuffer;
}

function customBase64Encode(buffer) {
  const base64Chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let result = '';
  const bytes = new Uint8Array(buffer);
  const len = bytes.byteLength;
  let i;
  for (i = 0; i < len - 2; i += 3) {
    result += base64Chars[bytes[i] >> 2];
    result += base64Chars[((bytes[i] & 0x3) << 4) | (bytes[i + 1] >> 4)];
    result += base64Chars[((bytes[i + 1] & 0xF) << 2) | (bytes[i + 2] >> 6)];
    result += base64Chars[bytes[i + 2] & 0x3F];
  }
  if (i === len - 1) {
    result += base64Chars[bytes[i] >> 2];
    result += base64Chars[(bytes[i] & 0x3) << 4];
    result += '==';
  } else if (i === len - 2) {
    result += base64Chars[bytes[i] >> 2];
    result += base64Chars[((bytes[i] & 0x3) << 4) | (bytes[i + 1] >> 4)];
    result += base64Chars[(bytes[i + 1] & 0xF) << 2];
    result += '=';
  }
  return result;
}

function renderTable(base64Input) {
  originalBase64Input = base64Input;
  originalFloatValues = {};
  console.log('cstruct raw:', JSON.stringify(cstruct.substring(0, 200)));
  globalCstructVars = parseCStruct(cstruct);
  globalBase64Array = base64ToArrayBuffer(base64Input);
  console.log('Parsed fields:', globalCstructVars.length, 'Buffer length:', globalBase64Array.length);



  const tableBody = document.getElementById('table-body');
  tableBody.innerHTML = '';
  let index = 0;

  globalCstructVars.forEach((varInfo, i) => {
    const varSize = varInfo.size;
    let value;

    if (varInfo.type === 'uint16_t') {
      value = globalBase64Array[index] | (globalBase64Array[index + 1] << 8);
    } else if (varInfo.type === 'int16_t') {
      value = new DataView(globalBase64Array.buffer).getInt16(index, true);
    } else if (varInfo.type === 'uint32_t') {
      value = globalBase64Array[index] | (globalBase64Array[index + 1] << 8) |
              (globalBase64Array[index + 2] << 16) | (globalBase64Array[index + 3] << 24);
    } else if (varInfo.type === 'float') {
      value = new DataView(globalBase64Array.buffer).getFloat32(index, true);
      originalFloatValues[varInfo.name] = {
        value: value,
        bytes: [globalBase64Array[index], globalBase64Array[index + 1], globalBase64Array[index + 2], globalBase64Array[index + 3]]
      };
    } else if ((varInfo.type === 'uint8_t' || varInfo.type === 'int8_t') && varInfo.arraySize) {
      value = [];
      for (let k = 0; k < varInfo.arraySize; k++) {
        if (varInfo.type === 'uint8_t') value.push(globalBase64Array[index + k]);
        else value.push(new DataView(globalBase64Array.buffer).getInt8(index + k));
      }
    } else if (varInfo.type === 'uint8_t') {
      value = globalBase64Array[index];
    } else if (varInfo.type === 'int8_t') {
      value = new DataView(globalBase64Array.buffer).getInt8(index);
    }

    const row = document.createElement('tr');
    const nameCell = document.createElement('td');
    const valueCell = document.createElement('td');

    const variableContainer = document.createElement('div');
    variableContainer.classList.add('variable-name');

    if (varInfo.comment) {
      const tooltip = document.createElement('div');
      tooltip.classList.add('tooltip');
      const lightbulb = document.createElement('span');
      lightbulb.classList.add('lightbulb');
      const tooltipText = document.createElement('span');
      tooltipText.classList.add('tooltiptext');
      tooltipText.textContent = varInfo.comment;
      tooltip.appendChild(lightbulb);
      tooltip.appendChild(tooltipText);
      variableContainer.appendChild(tooltip);
    }

    const nameSpan = document.createElement('span');
    nameSpan.textContent = varInfo.name + (Array.isArray(value) ? '[' + varInfo.arraySize + ']' : '') + ' (' + varInfo.size + ' bytes)';
    variableContainer.appendChild(nameSpan);
    nameCell.appendChild(variableContainer);

    const valueInput = document.createElement('input');
    valueInput.type = 'text';
    if (Array.isArray(value)) {
      valueInput.value = value.map(b => b.toString(16).padStart(2, '0')).join(' ');
    } else if (varInfo.type === 'float') {
      valueInput.value = value.toString();
    } else {
      valueInput.value = value;
    }
    valueInput.dataset.type = varInfo.type;
    valueInput.dataset.index = i;
    valueInput.dataset.name = varInfo.name;
    if (varInfo.arraySize) valueInput.dataset.arraySize = varInfo.arraySize;
    valueCell.appendChild(valueInput);

    row.appendChild(nameCell);
    row.appendChild(valueCell);
    tableBody.appendChild(row);

    index += varSize;
  });
}

function buildBase64FromInputs() {
  const totalSize = globalCstructVars.reduce((sum, v) => sum + v.size, 0);
  const newBuffer = new ArrayBuffer(totalSize);
  const newArray = new Uint8Array(newBuffer);
  const dataView = new DataView(newBuffer);

  const inputs = document.querySelectorAll('#table-body input');
  let offset = 0;

  inputs.forEach((input) => {
    const type = input.dataset.type;
    const index = parseInt(input.dataset.index, 10);
    const varInfo = globalCstructVars[index];
    const varName = input.dataset.name;
    const value = input.value;

    if ((type === 'uint8_t' || type === 'int8_t') && varInfo.arraySize) {
      const hexValues = value.trim().split(/\s+/);
      for (let i = 0; i < varInfo.arraySize; i++) {
        if (i < hexValues.length) newArray[offset + i] = parseInt(hexValues[i], 16) || 0;
        else newArray[offset + i] = 0;
      }
    } else if (type === 'uint8_t') {
      newArray[offset] = parseInt(value, 10) & 0xFF;
    } else if (type === 'int8_t') {
      dataView.setInt8(offset, parseInt(value, 10));
    } else if (type === 'uint16_t') {
      const numValue = parseInt(value, 10);
      newArray[offset] = numValue & 0xFF;
      newArray[offset + 1] = (numValue >> 8) & 0xFF;
    } else if (type === 'int16_t') {
      dataView.setInt16(offset, parseInt(value, 10), true);
    } else if (type === 'uint32_t') {
      const numValue = parseInt(value, 10);
      newArray[offset] = numValue & 0xFF;
      newArray[offset + 1] = (numValue >> 8) & 0xFF;
      newArray[offset + 2] = (numValue >> 16) & 0xFF;
      newArray[offset + 3] = (numValue >> 24) & 0xFF;
    } else if (type === 'float') {
      const floatValue = parseFloat(value);
      if (originalFloatValues[varName] && Math.abs(floatValue - originalFloatValues[varName].value) < 1e-10) {
        newArray[offset] = originalFloatValues[varName].bytes[0];
        newArray[offset + 1] = originalFloatValues[varName].bytes[1];
        newArray[offset + 2] = originalFloatValues[varName].bytes[2];
        newArray[offset + 3] = originalFloatValues[varName].bytes[3];
      } else {
        dataView.setFloat32(offset, floatValue, true);
      }
    }
    offset += varInfo.size;
  });

  return customBase64Encode(newBuffer);
}

function setStatus(msg, isError) {
  const el = document.getElementById('status');
  el.textContent = msg;
  el.style.color = isError ? '#b00000' : '#006400';
}

function loadConfig() {
  fetch('/getconf')
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.text();
    })
    .then(b64 => {
      console.log('Base64 string length:', b64.trim().length, 'Decoded bytes (approx):', Math.floor(b64.trim().length * 3 / 4));
      renderTable(b64.trim());
      setStatus('Configuration loaded from device.', false);
    })
    .catch(err => setStatus('Failed to load configuration: ' + err, true));
}

function saveConfig() {
  const b64 = buildBase64FromInputs();
  fetch('/setconf', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'data=' + encodeURIComponent(b64)
  })
  .then(r => {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.text();
  })
  .then(() => {
    document.getElementById('confirm-modal').style.display = 'flex';
  })
  .catch(err => setStatus('Failed to save configuration: ' + err, true));
}

function closeModal() {
  document.getElementById('confirm-modal').style.display = 'none';
}

function applyConfig() {
  fetch('/applyconf')
    .then(() => {
      closeModal();
      setStatus('Configuration applied.', false);
    })
    .catch(err => setStatus('Failed to apply configuration: ' + err, true));
}

function rebootConfig() {
  fetch('/rebootdevice').finally(() => {
    closeModal();
    setStatus('Rebooting device...', false);
  });
}

window.onload = loadConfig;
</script>
</body>
</html>
)de4f";