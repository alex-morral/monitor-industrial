/**
 * ============================================================
 *  Monitor Industrial — Backend Node.js v1.0
 *  Puente MQTT → Socket.io en tiempo real
 * ============================================================
 */

'use strict';

require('dotenv').config();

const express   = require('express');
const http      = require('http');
const path      = require('path');
const mqtt      = require('mqtt');
const { Server } = require('socket.io');
const TelegramBot = require('node-telegram-bot-api');

// ============================================================
//  TELEGRAM
// ============================================================
const TELEGRAM_TOKEN   = process.env.TELEGRAM_TOKEN;
const TELEGRAM_CHAT_ID = process.env.TELEGRAM_CHAT_ID;
const bot = new TelegramBot(TELEGRAM_TOKEN, { polling: false });

let alarmaActiva = false;  // Evita enviar múltiples alertas seguidas

function enviarAlertaTelegram(temp) {
  const msg = `🚨 ALARMA TÉRMICA\n\nTemperatura del disipador: ${temp}°C\nRelé activado — carga desconectada.\n\n⏱ ${new Date().toLocaleTimeString('es-ES')}`;
  bot.sendMessage(TELEGRAM_CHAT_ID, msg).catch(err => console.error('[Telegram] Error:', err.message));
}

function enviarResetTelegram(temp) {
  const msg = `✅ ALARMA RESUELTA\n\nTemperatura normalizada: ${temp}°C\nRelé desactivado — carga reconectada.\n\n⏱ ${new Date().toLocaleTimeString('es-ES')}`;
  bot.sendMessage(TELEGRAM_CHAT_ID, msg).catch(err => console.error('[Telegram] Error:', err.message));
}

// ============================================================
//  CONFIGURACIÓN
// ============================================================
const PORT           = process.env.PORT || 3000;
const MQTT_BROKER    = process.env.MQTT_BROKER || 'mqtt://broker.hivemq.com';
const MQTT_PORT      = 1883;
const TOPIC_TEL      = 'industrial/monitor/telemetria';
const TOPIC_CMD      = 'industrial/monitor/cmd';

// ============================================================
//  SERVIDOR HTTP + SOCKET.IO
// ============================================================
const app    = express();
const server = http.createServer(app);
const io     = new Server(server, {
  cors: { origin: '*' },
  pingTimeout: 20000,
  pingInterval: 10000,
});

// Servir frontend estático
app.use(express.static(path.join(__dirname, '..', 'frontend')));
app.use(express.json());

// ============================================================
//  ESTADO INTERNO DEL SERVIDOR
// ============================================================
const stats = {
  mensajesRecibidos : 0,
  erroresJson       : 0,
  ultimaRecepcion   : null,
  mqttConectado     : false,
  brokerUrl         : MQTT_BROKER,
  clientesWS        : 0,
};

// Último payload para clientes que se conecten tarde
let ultimaTelemetria = null;

// ============================================================
//  CLIENTE MQTT
// ============================================================
const mqttClient = mqtt.connect(MQTT_BROKER, {
  port           : MQTT_PORT,
  clientId       : `backend-monitor-${Math.random().toString(16).slice(2, 8)}`,
  clean          : true,
  reconnectPeriod: 5_000,    // Reconectar cada 5s si se pierde conexión
  connectTimeout : 30_000,
  keepalive      : 60,
});

mqttClient.on('connect', () => {
  stats.mqttConectado = true;
  console.log(`[MQTT] ✓ Conectado → ${MQTT_BROKER}`);

  mqttClient.subscribe(TOPIC_TEL, { qos: 0 }, (err) => {
    if (err) return console.error('[MQTT] Error al suscribirse:', err.message);
    console.log(`[MQTT] Suscrito → ${TOPIC_TEL}`);
  });

  // Notificar a todos los clientes WebSocket
  io.emit('mqtt_status', { conectado: true });
});

mqttClient.on('reconnect', () => {
  stats.mqttConectado = false;
  console.log('[MQTT] Reconectando...');
  io.emit('mqtt_status', { conectado: false });
});

mqttClient.on('offline', () => {
  stats.mqttConectado = false;
  console.log('[MQTT] Offline');
  io.emit('mqtt_status', { conectado: false });
});

mqttClient.on('error', (err) => {
  stats.mqttConectado = false;
  console.error('[MQTT] Error:', err.message);
});

mqttClient.on('message', (topic, payloadBuffer) => {
  let data;

  // Parsear JSON con manejo de errores robusto
  try {
    data = JSON.parse(payloadBuffer.toString('utf8'));
  } catch (e) {
    stats.erroresJson++;
    console.error(`[MQTT] JSON inválido (error #${stats.erroresJson}):`, e.message);
    return;
  }

  // Actualizar estadísticas
  stats.mensajesRecibidos++;
  stats.ultimaRecepcion = new Date().toISOString();
  ultimaTelemetria = data;

  // Broadcast a TODOS los clientes WebSocket conectados en tiempo real
  io.emit('telemetria', data);

  // ── Notificaciones Telegram
  const prot = data?.estado?.proteccion_termica_disparada === true;
  const temp = data?.termico?.temp_disipador_c ?? '?';
  if (prot && !alarmaActiva) {
    alarmaActiva = true;
    enviarAlertaTelegram(temp);
    console.log('[Telegram] Alerta de alarma enviada');
  } else if (!prot && alarmaActiva) {
    alarmaActiva = false;
    enviarResetTelegram(temp);
    console.log('[Telegram] Alerta de reset enviada');
  }

  // Log periódico (cada 30 mensajes = ~30s)
  if (stats.mensajesRecibidos % 30 === 0) {
    const uptime = data?.meta?.tiempo_encendido_s ?? '?';
    const t_dis  = data?.termico?.temp_disipador_c ?? '?';
    const ef     = data?.potencia?.eficiencia_calculada_pct ?? '?';
    console.log(`[TEL] #${stats.mensajesRecibidos} | Uptime: ${uptime}s | T_dis: ${t_dis}°C | η: ${ef}%`);
  }
});

// ============================================================
//  REST API
// ============================================================

// Enviar comandos al ESP32 via MQTT
app.post('/api/cmd', (req, res) => {
  const { comando } = req.body;
  if (!comando || typeof comando !== 'string') {
    return res.status(400).json({ error: 'Campo "comando" requerido (string)' });
  }
  if (comando.length > 64) {
    return res.status(400).json({ error: 'Comando demasiado largo (máx 64 chars)' });
  }

  mqttClient.publish(TOPIC_CMD, comando, { qos: 1 }, (err) => {
    if (err) return res.status(502).json({ error: 'MQTT publish falló', detalle: err.message });
    console.log(`[CMD] → ESP32: "${comando}"`);
    res.json({ ok: true, comando, timestamp: new Date().toISOString() });
  });
});

// Estado del servidor (útil para monitorización)
app.get('/api/status', (_req, res) => {
  res.json({
    ...stats,
    clientesWS  : io.engine.clientsCount,
    uptimeServer: Math.floor(process.uptime()),
  });
});

// Último payload recibido (para debug)
app.get('/api/ultimo', (_req, res) => {
  if (!ultimaTelemetria) return res.status(204).send();
  res.json(ultimaTelemetria);
});

// ============================================================
//  SOCKET.IO — Gestión de conexiones WebSocket
// ============================================================
io.on('connection', (socket) => {
  stats.clientesWS++;
  console.log(`[WS] + Cliente conectado: ${socket.id} | Total: ${stats.clientesWS}`);

  // Enviar estado inmediato al cliente recién conectado
  socket.emit('mqtt_status', { conectado: stats.mqttConectado });
  if (ultimaTelemetria) {
    socket.emit('telemetria', ultimaTelemetria);  // Datos inmediatos (no esperar 1s)
  }

  // Recibir comandos desde el dashboard web
  socket.on('send_cmd', (cmd) => {
    if (typeof cmd !== 'string' || cmd.length === 0) return;
    mqttClient.publish(TOPIC_CMD, cmd, { qos: 1 });
    console.log(`[WS→MQTT] Cmd desde "${socket.id}": "${cmd}"`);
  });

  socket.on('disconnect', (reason) => {
    stats.clientesWS = Math.max(0, stats.clientesWS - 1);
    console.log(`[WS] - Cliente desconectado: ${socket.id} (${reason}) | Total: ${stats.clientesWS}`);
  });
});

// ============================================================
//  ARRANQUE
// ============================================================
server.listen(PORT, () => {
  console.log('');
  console.log('╔══════════════════════════════════════════════╗');
  console.log('║   Monitor Industrial — Backend Node.js v1.0  ║');
  console.log(`║   Dashboard:   http://localhost:${PORT}           ║`);
  console.log(`║   Broker MQTT: ${MQTT_BROKER.padEnd(30)} ║`);
  console.log(`║   Topic Tel.:  ${TOPIC_TEL.padEnd(30)} ║`);
  console.log('╚══════════════════════════════════════════════╝');
  console.log('');
});

// Graceful shutdown
process.on('SIGINT', () => {
  console.log('\n[SHUTDOWN] Cerrando conexiones...');
  mqttClient.end(true, () => {
    server.close(() => {
      console.log('[SHUTDOWN] Servidor detenido limpiamente.');
      process.exit(0);
    });
  });
});
