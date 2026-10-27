const $ = id => document.getElementById(id);
function fmt(value, digits=2, suffix='') { return value === null || value === undefined || !Number.isFinite(Number(value)) ? '—' : Number(value).toFixed(digits) + suffix; }
function setAlert(data) {
  const fdcAlert = data.fdcWaterSuspectedAlert === true;
  const turbAlert = data.sensorWindowAlert === true;
  const badge = $('alertBadge');
  if (fdcAlert) {
    $('alertTitle').textContent = 'WATER CONTAMINATION SUSPECTED*';
    $('alertDescription').textContent = 'FDC average is at/below the recorded 1.0 mL endpoint reference band. This is a preliminary threshold indication; intermediate readings were non-monotonic, so contamination is not confirmed.';
    badge.textContent = 'FDC THRESHOLD'; badge.className = 'badge active';
  } else if (turbAlert) {
    $('alertTitle').textContent = 'PROVISIONAL TURBIDITY-WINDOW ALERT';
    $('alertDescription').textContent = 'The analog voltage entered the provisional water-like window. This is not confirmation of E20 contamination or phase separation.';
    badge.textContent = 'TURBIDITY WINDOW'; badge.className = 'badge active';
  } else {
    $('alertTitle').textContent = 'Monitoring';
    $('alertDescription').textContent = 'No threshold alert active. Absence of an alert does not prove the fuel is uncontaminated.';
    badge.textContent = 'NO ALERT'; badge.className = 'badge neutral';
  }
}
async function refresh() {
  try {
    const response = await fetch('/data?ts=' + Date.now(), {cache:'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const d = await response.json();
    $('connection').textContent = 'ESP32 connected'; document.querySelector('.dot').style.background = 'var(--green)';
    $('waterPercent').textContent = d.waterPercent == null ? 'N/A*' : fmt(d.waterPercent,1,'%*');
    $('waterNote').textContent = d.waterPercent == null ? 'No validated intermediate estimate' : 'Endpoint-only nominal label';
    $('fdcAvg').textContent = fmt(d.fdcAvg32,0);
    $('fdcRange').textContent = d.fdcWaterSuspectedAlert ? 'Below 1.0 mL reference-band threshold*' : (d.fdcWithinObservedRange ? 'Inside observed endpoint range' : 'Outside / awaiting endpoint range');
    $('turbV').textContent = fmt(d.turbidityVoltageV,3,' V');
    $('turbRaw').textContent = 'ADC raw counts: ' + (d.turbidityRaw >= 0 ? d.turbidityRaw : '—') + ' (not NTU)';
    $('temperature').textContent = fmt(d.temperatureC,1,' °C');
    $('updated').textContent = 'Updated ' + new Date().toLocaleTimeString();
    setAlert(d);
    const rows = [
      `FDC2214 initialized/read valid: ${d.fdcOK}`, `DS18B20 reading valid: ${d.temperatureOK}`,
      `Analog input sampled: ${d.turbidityOK} (sampling does not prove probe integrity)`,
      `OLED initialized: ${d.oledOK}`, `MAX7219 matrix initialized: ${d.matrixOK}`,
      `Alert source: ${d.alertSource || 'NONE'}`, `Uptime: ${d.uptimeSeconds} seconds`,
      `Measurement note: ${d.measurementNote}`
    ];
    $('healthList').replaceChildren(...rows.map(t => { const li=document.createElement('li'); li.textContent=t; return li; }));
  } catch (e) {
    $('connection').textContent = 'Connection lost'; document.querySelector('.dot').style.background = 'var(--red)';
  }
}
refresh(); setInterval(refresh, 1000);
