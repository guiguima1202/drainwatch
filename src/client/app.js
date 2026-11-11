const INTERVALO_MS = 2000;
const TIMEOUT_MS = 3000;
const FALHAS_PARA_OFFLINE = 2;

const ROTULOS = { normal: "Normal", atencao: "Atenção", critico: "Crítico", erro: "Sensor sem leitura" };
const TEXTO_BLOQUEIO = { sem_agua: "Pouca água para a bomba", tempo_maximo: "Tempo máximo atingido", sensor: "Sensor sem leitura" };
const BOTOES = ["btn-bomba-on", "btn-bomba-off", "btn-bomba-auto", "btn-log", "btn-limpar-log"];

let ip = lerIpSalvo();
let conexao = "aguardando";
let falhasSeguidas = 0;
let statusAnterior = null;
let ultimaSincHora = 0;
let timer = null;

const $ = (id) => document.getElementById(id);

async function buscar(caminho) {
  const controle = new AbortController();
  const limite = setTimeout(() => controle.abort(), TIMEOUT_MS);
  try {
    const resp = await fetch("http://" + ip + caminho, { signal: controle.signal });
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    return resp;
  } finally {
    clearTimeout(limite);
  }
}
async function buscarJson(caminho) { return (await buscar(caminho)).json(); }

async function atualizar() {
  if (!ip) return;
  try {
    const dados = await buscarJson("/api/status");
    marcarOnline();
    mostrar(dados);
  } catch (erro) {
    falhasSeguidas++;
    if (falhasSeguidas >= FALHAS_PARA_OFFLINE) marcarOffline();
  }
}

function agendar() {
  clearTimeout(timer);
  timer = setTimeout(async () => { await atualizar(); agendar(); }, INTERVALO_MS);
}

async function enviarComando(tipo, estado) {
  try {
    const dados = await buscarJson("/api/" + tipo + "?estado=" + estado);
    mostrar(dados);
    registrarEvento("Comando enviado: " + tipo + " " + estado);
    carregarLog();
  } catch (erro) {
    registrarEvento("Falha ao enviar o comando: o ESP32 não respondeu");
  }
}

async function sincronizarHora() {
  ultimaSincHora = Date.now();
  try {
    await buscar("/api/hora?t=" + Math.floor(Date.now() / 1000));
    registrarEvento("Hora enviada ao ESP32");
  } catch (erro) {  }
}

async function carregarLog() {
  try {
    const texto = await (await buscar("/api/log")).text();
    const caixa = $("log-texto");
    caixa.textContent = texto.trim() || "Log vazio.";
    caixa.scrollTop = caixa.scrollHeight;
  } catch (erro) {
    $("log-texto").textContent = "Não foi possível ler o log.";
  }
}

async function limparLog() {
  try {
    await buscar("/api/log/limpar");
    registrarEvento("Log do ESP32 apagado");
    carregarLog();
  } catch (erro) {
    registrarEvento("Falha ao limpar o log");
  }
}

function marcarOnline() {
  falhasSeguidas = 0;
  if (conexao !== "online") {
    conexao = "online";
    registrarEvento("Conectado ao ESP32");
    carregarLog();
  }
  document.body.dataset.conexao = "online";
  $("selo-conexao").textContent = "ESP32 conectado";
  alternarBotoes(true);
}

function marcarOffline() {
  if (conexao !== "offline") {
    conexao = "offline";
    registrarEvento("O ESP32 não responde");
  }
  document.body.dataset.conexao = "offline";
  document.body.dataset.status = "sem-dados";
  $("selo-conexao").textContent = "ESP32 offline";
  $("selo-status").textContent = "Sem conexão";
  alternarBotoes(false);
}

function alternarBotoes(ligado) {
  BOTOES.forEach((id) => { $(id).disabled = !ligado; });
}

function marcarBotao(id, ativo) { $(id).setAttribute("aria-pressed", String(ativo)); }

function mostrar(d) {
  const pct = Math.max(0, d.percentual);
  $("percentual").textContent = Math.round(pct);
  $("nivel").textContent = d.nivel_cm.toFixed(1);
  $("distancia").textContent = d.distancia_cm.toFixed(1);
  $("sinal").textContent = d.wifi_rssi ? d.wifi_rssi + " dBm" : "Ponto de acesso";
  $("uptime").textContent = formatarTempo(d.uptime_s);
  $("hora").textContent = new Date().toLocaleTimeString("pt-BR");

  document.body.dataset.status = d.status;
  $("selo-status").textContent = ROTULOS[d.status] || d.status;
  $("agua").style.height = Math.min(pct, 100) + "%";
  $("tanque").setAttribute("aria-label", "Nível de água em " + Math.round(pct) + " por cento");

  const b = d.bomba;
  $("bomba-estado").textContent = b.estado === "ligada" ? "Ligada" : "Desligada";
  $("modo-bomba").textContent = b.modo === "auto" ? "Automático" : "Manual";
  $("bomba-tempo").textContent = b.estado === "ligada" ? b.tempo_ligada_s + " s" : "--";
  $("bomba-bloqueio").textContent = TEXTO_BLOQUEIO[b.bloqueio] || "Nenhuma";
  marcarBotao("btn-bomba-auto", b.modo === "auto");
  marcarBotao("btn-bomba-on", b.modo === "manual" && b.estado === "ligada");
  marcarBotao("btn-bomba-off", b.modo === "manual" && b.estado === "desligada");

  $("ip-esp").textContent = d.ip;
  $("mac").textContent = d.mac;
  $("rede").textContent = (d.rede === "estacao" ? "Wi-Fi: " : "Ponto de acesso: ") + d.ssid;
  $("porta").textContent = d.porta;
  $("hora-esp").textContent = d.hora_sincronizada ? d.hora : "Não sincronizada";

  if (!d.hora_sincronizada && Date.now() - ultimaSincHora > 10000) sincronizarHora();

  if (statusAnterior !== null && d.status !== statusAnterior) {
    registrarEvento("Status mudou para " + (ROTULOS[d.status] || d.status) + " (" + Math.round(pct) + "%)");
    carregarLog();
  }
  statusAnterior = d.status;
}

function registrarEvento(texto) {
  const lista = $("lista-eventos");
  if (lista.children.length === 1 && lista.children[0].textContent === "Nenhum evento ainda.") {
    lista.innerHTML = "";
  }
  const item = document.createElement("li");
  const hora = document.createElement("time");
  hora.textContent = new Date().toLocaleTimeString("pt-BR");
  item.appendChild(hora);
  item.appendChild(document.createTextNode(texto));
  lista.prepend(item);
  while (lista.children.length > 50) lista.lastChild.remove();
}

function formatarTempo(seg) {
  const h = Math.floor(seg / 3600), m = Math.floor((seg % 3600) / 60), s = seg % 60;
  return (h ? h + " h " : "") + (m || h ? m + " min " : "") + s + " s";
}

function lerIpSalvo() {
  try { return localStorage.getItem("drainwatch_ip") || ""; } catch (e) { return ""; }
}
function salvarIp(valor) {
  try { localStorage.setItem("drainwatch_ip", valor); } catch (e) {}
}

$("form-ip").addEventListener("submit", (evento) => {
  evento.preventDefault();
  const valor = $("campo-ip").value.trim().replace(/^https?:\/\//, "").replace(/\/$/, "");
  if (!/^[\w.\-]+(:\d+)?$/.test(valor)) {
    registrarEvento("Endereço inválido. Use algo como 192.168.0.50");
    return;
  }
  ip = valor;
  salvarIp(ip);
  conexao = "aguardando";
  falhasSeguidas = 0;
  statusAnterior = null;
  registrarEvento("Endereço do ESP32: " + ip);
  atualizar().then(agendar);
});

$("btn-bomba-on").addEventListener("click", () => enviarComando("bomba", "on"));
$("btn-bomba-off").addEventListener("click", () => enviarComando("bomba", "off"));
$("btn-bomba-auto").addEventListener("click", () => enviarComando("bomba", "auto"));
$("btn-log").addEventListener("click", carregarLog);
$("btn-limpar-log").addEventListener("click", limparLog);

$("campo-ip").value = ip;
if (ip) {
  atualizar().then(agendar);
} else {
  $("selo-conexao").textContent = "Informe o endereço do ESP32";
}
