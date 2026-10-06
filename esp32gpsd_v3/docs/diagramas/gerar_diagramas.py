#!/usr/bin/env python3
"""Gera atlas DrawIO v3 sem dependências; exportar com DrawIO Desktop.

Coordenadas explícitas mantêm mapas auditáveis e cada bloco editável.
"""
from pathlib import Path
import html
import xml.etree.ElementTree as E

OUT = Path(__file__).resolve().parent
PAL = {'sensor':('#EAF3FF','#2864A0'), 'radio':('#F0ECFB','#7157A0'),
       'sd':('#E7F3ED','#287452'), 'warn':('#FFF2D9','#A36A15'),
       'base':('#EFF3F8','#354D6B'), 'http':('#E4F3F4','#167580')}
PAGES=[]
class Page:
    def __init__(self, slug, title, subtitle, source):
        self.slug=slug; self.n=0; self.boxes={}
        self.diagram=E.Element('diagram',id=slug,name=title)
        model=E.SubElement(self.diagram,'mxGraphModel',dx='1600',dy='1100',grid='1',gridSize='10',guides='1',tooltips='1',connect='1',arrows='1',fold='1',page='1',pageScale='1',pageWidth='1600',pageHeight='1100',math='0',shadow='0',background='#FFFFFF')
        self.root=E.SubElement(model,'root');E.SubElement(self.root,'mxCell',id='0');E.SubElement(self.root,'mxCell',id='1',parent='0')
        self.rect('sheet','',0,0,1600,1100,'fillColor=#FFFFFF;strokeColor=none;')
        self.rect('accent','',40,38,8,91,'fillColor=#2864A0;strokeColor=none;')
        self.label('kicker','ESP32 GPS LOGGER / V3 / ATLAS TÉCNICO',64,32,1450,25,14,'#2864A0','DejaVu Sans Mono')
        self.label('title',title,64,61,1450,43,30,'#17324F','Ubuntu Sans',True)
        self.label('sub',subtitle,64,111,1450,37,16,'#52667B')
        self.rect('footerline','',48,1006,1504,1,'fillColor=#DCE4ED;strokeColor=none;')
        self.label('source','Fonte: '+source,48,1021,1500,25,13,'#52667B','DejaVu Sans Mono')
        self.label('scope','SENSOR  azul     RÁDIO  violeta     SD  verde     HTTP  petróleo     CONDIÇÃO / FALHA  âmbar  •  Snapshot 06/10/2026',48,1050,1500,24,13,'#52667B')
        PAGES.append(self)
    def rect(self,id,value,x,y,w,h,style):
        c=E.SubElement(self.root,'mxCell',id=id,value=value,style=style,vertex='1',parent='1')
        E.SubElement(c,'mxGeometry',x=str(x),y=str(y),width=str(w),height=str(h),attrib={'as':'geometry'})
        self.boxes[id]=(x,y,w,h);return id
    def label(self,id,text,x,y,w,h,size=16,color='#17324F',font='Noto Sans',bold=False):
        return self.rect(id,html.escape(text),x,y,w,h,f'text;html=1;whiteSpace=wrap;align=left;verticalAlign=middle;spacing=0;fontFamily={font};fontSize={size};fontColor={color};fontStyle={1 if bold else 0};')
    def node(self,id,title,body,x,y,w=300,h=135,kind='base',shape='rect'):
        bg,stroke=PAL[kind]
        value='<b>'+html.escape(title)+'</b><br><br>'+html.escape(body).replace('\n','<br>')
        st=f'rounded=1;arcSize=10;whiteSpace=wrap;html=1;fillColor={bg};strokeColor={stroke};strokeWidth=1.5;fontColor=#17324F;fontFamily=Noto Sans;fontSize=17;align=left;verticalAlign=middle;spacingLeft=18;spacingRight=18;spacingTop=12;spacingBottom=12;'
        if shape=='diamond':st+='shape=rhombus;align=center;spacingLeft=8;spacingRight=8;'
        return self.rect(id,value,x,y,w,h,st)
    def note(self,id,title,body,x,y,w,h=100,kind='warn'):
        return self.node(id,title,body,x,y,w,h,kind)
    def lane(self,id,title,x,y,w,h,kind='base'):
        bg,stroke=PAL[kind]
        self.rect(id,'',x,y,w,h,f'rounded=1;arcSize=4;fillColor={bg};fillOpacity=35;strokeColor={stroke};strokeOpacity=35;strokeWidth=1;')
        self.label(id+'-title',title,x+18,y+12,w-36,30,17,stroke,'DejaVu Sans Mono',True)
    def edge(self,a,b,label='',kind='base',side=None,points=None,dashed=False):
        self.n+=1; stroke=PAL[kind][1]
        style=f'edgeStyle=orthogonalEdgeStyle;rounded=0;html=1;endArrow=block;endFill=1;strokeWidth=1.7;strokeColor={stroke};fontFamily=Noto Sans;fontSize=13;fontColor=#17324F;labelBackgroundColor=#FFFFFF;'
        if dashed:style+='dashed=1;dashPattern=6 4;'
        if side:
            ex,ey,ix,iy=side;style+=f'exitX={ex};exitY={ey};entryX={ix};entryY={iy};exitPerimeter=1;entryPerimeter=1;'
        c=E.SubElement(self.root,'mxCell',id='e'+str(self.n),value=html.escape(label),style=style,edge='1',parent='1',source=a,target=b)
        geo=E.SubElement(c,'mxGeometry',relative='1',attrib={'as':'geometry'})
        if points:
            arr=E.SubElement(geo,'Array',attrib={'as':'points'})
            for x,y in points:E.SubElement(arr,'mxPoint',x=str(x),y=str(y))
        return c

p=Page('01-visao-geral','Arquitetura integrada','Do ambiente físico ao CSV: aquisição local, observação de rádio e download enquanto parado.','esp32gpsd_v3.ino:1–233, 1344–1547; README.md')
p.lane('s','ENTRADAS / AMBIENTE',48,175,320,750,'sensor');p.lane('f','FIRMWARE / ESP32',418,175,690,750);p.lane('o','SAÍDAS / OPERADOR',1158,175,394,750,'sd')
p.node('gps','GPS / UART2','Posição, hora UTC, RMC/GGA\n9600 baud • RX 1024 B',68,239,280,135,'sensor')
p.node('sensor','DHT22 + MPU6050','Temperatura / umidade\nAceleração / giroscópio',68,438,280,135,'sensor')
p.node('rf','Redes WiFi + BLE','Observação de redes e anúncios\nScans coordenados por modo',68,650,280,135,'radio')
p.node('loop','loopTask / core 1','Parser + sensores + modos\nConsumo BLE e escrita SD',445,300,290,150)
p.node('buf','Buffers circulares','GPS 150 • WiFi 50 • BLE 50\nLotes temporários no heap',785,300,290,150,'sd')
p.node('radio','Scanners / fila BLE','WiFi assíncrono\nNimBLE → fila 10 → loopTask',445,650,290,150,'radio')
p.node('http','Hotspot_HTTP / core 1','WebServer :80 • AP parado\nLeitura do SD via sdMutex',785,650,290,150,'http')
p.node('sd','microSD / SdFat','/log.txt • /wifi.txt • /ble.txt\nFAT16 / FAT32 / exFAT',1190,300,330,150,'sd')
p.node('user','Celular / computador','192.168.4.1\nAtualizar página / baixar logs',1190,650,330,150,'http')
p.edge('gps','loop','NMEA','sensor');p.edge('sensor','loop','leituras','sensor');p.edge('rf','radio','scan','radio');p.edge('radio','loop','resultados / fila','radio',side=(.5,0,.5,1));p.edge('loop','buf','linhas CSV','sd');p.edge('buf','sd','flush confirmado','sd');p.edge('sd','http','arquivo original','sd',side=(.5,1,.5,0),points=[(1355,545),(930,545)]);p.edge('http','user','HTTP / download','http')
p.note('n','Coleta local, sem backend','WiFi STA faz scan; AP oferece arquivos. GSM, nuvem e BT clássico não integram a v3.',445,845,1075,65)

p=Page('02-hardware','Hardware e interligações','Pinagem do ESP32 Dev Module; UART cruzada e sinais SPI separados de alimentação.','esp32gpsd_v3.ino:20–32, 1344–1391; ../docs/wiki/dados-hardware.md')
p.node('esp','ESP32 Dev Module','UART2: RX17 / TX16\nDHT: GPIO32\nI²C: SDA21 / SCL22\nSPI: CS5 / SCK18 / MOSI23 / MISO19\nRádio WiFi + BLE integrado',600,300,390,320)
p.node('gps','Módulo GPS','TX GPS → RX17 ESP32\nRX GPS ← TX16 ESP32\nNMEA • 9600 baud • 8N1',70,220,350,165,'sensor')
p.node('dht','DHT22','DATA ↔ GPIO32\nProtocolo digital DHT\nNão é Dallas/Maxim 1-Wire',70,525,350,165,'sensor')
p.node('imu','MPU6050 / opcional','SDA ↔ GPIO21 • SCL ← GPIO22\nI²C padrão • ±16 g\n±1000 °/s • filtro 44 Hz',1150,220,380,165,'sensor')
p.node('sd','Módulo microSD','CS ← GPIO5 • SCK ← GPIO18\nMOSI ← GPIO23 • MISO → GPIO19\nSPI compartilhado • 16 MHz',1150,525,380,165,'sd')
p.node('pwr','Alimentação / GND comum','Lógica ESP32: 3,3 V. Tensão de alimentação depende do módulo.\nFonte, regulador, pull-ups e desacoplamento não possuem esquema/BOM neste projeto.',70,810,1000,125,'warn')
p.node('usb','Host / USB serial','Programação do ESP32\nConsole: 115200 baud',1150,810,380,125)
p.edge('gps','esp','TX GPS → RX17','sensor',side=(1,.35,0,.18));p.edge('esp','gps','TX16 → RX GPS','sensor',side=(0,.35,1,.8));p.edge('dht','esp','DATA GPIO32','sensor',side=(1,.5,0,.82));p.edge('esp','imu','I²C 21 / 22','sensor',side=(1,.18,0,.5));p.edge('esp','sd','SPI 5 / 18 / 23 / 19','sd',side=(1,.82,0,.5));p.edge('pwr','esp','VCC / GND: validar módulos','warn',side=(.7,0,.5,1),dashed=True);p.edge('usb','esp','USB / programação e console',side=(.5,0,.9,1),points=[(1340,760),(950,760)])
p.label('limits','SPI 18/19/23 e I²C 21/22 são padrões da placa documentada; não há remapeamento explícito no sketch.',70,736,1460,40,17)

p=Page('03-inicializacao','Inicialização e recuperação de boot','setup() só entrega controle ao loop após montar SD e preparar recursos essenciais.','esp32gpsd_v3.ino:709–731, 1333–1453')
items=[('serial','Serial + GPS','115200 / 9600 baud\nTZ=UTC0 • UART RX 1024 B','sensor'),('ram','Alocar RAM + mutex','3 buffers • sdMutex\nFalha → espera infinita','sd'),('sens','Iniciar sensores','DHT22; MPU opcional\nFalha MPU → sem IMU','sensor'),('mount','Montar microSD','sd.begin(SD_CONFIG)\nRetry infinito a cada 500 ms','sd'),('files','Preparar arquivos','Cabeçalho se log.txt não existe\nConferir existência dos logs','sd'),('wdt','Configurar watchdog','60 s • reconfigure no core 3.x\nRegistrar tarefa atual','base'),('boot','Registrar boot','bootCount++ • reset_reason\n# BOOT em log.txt','sd'),('ble','Criar fila / iniciar BLE','10 ponteiros • callbacks\nFalha fila → espera infinita','radio'),('move','Entrar em movimento','WIFI_STA • sem conexão\nNão inicia scan imediato','radio'),('web','Preparar HTTP','Rotas / e /download\nTarefa core 1, 8192 B, prio 2','http'),('run','Aguardar GPS / loop()','Scans em movimento dependem\nde RMC com velocidade válida','base')]
coords=[(60,195),(570,195),(1080,195),(1080,390),(570,390),(60,390),(60,585),(570,585),(1080,585),(1080,780),(570,780)]
for (id,t,b,k),(x,y) in zip(items,coords):p.node(id,t,b,x,y,440,135,k)
for i in range(len(items)-1):p.edge(items[i][0],items[i+1][0])
p.note('failure','Falha da tarefa HTTP','Logger continua; hotspotDisponivel=false.\nCHECK passa direto ao SONO.',60,780,440,135)

p=Page('04-loop','Ciclo principal e prioridades','Aquisição ≈1 s, seguida de serviço de modo, retry SD e resumo; foco HTTP tem retorno antecipado.','esp32gpsd_v3.ino:1459–1547')
p.node('start','Entrada de loop()','hotspotEmFoco()?\nAP aberto + download ou HTTP <8 s',70,190,400,130,'http')
p.node('focus','Foco HTTP / retorno','Descartar bytes UART GPS\nAlimentar WDT • delay 50 ms\nRetornar sem etapas seguintes',1090,190,440,150,'warn')
p.node('acq','GPS / MPU • janela 1 s','receberGPS(c) • drenarFilaBLE()\nMPU a cada 100 ms • WDT\nvTaskDelay(1) por passagem',70,405,400,155,'sensor')
p.node('data','Média + dados','Média das amostras MPU\nlerDHT() ≥2 s\nnewData → processarDadosGPS()',590,405,400,155,'sensor')
p.node('mode','Modo / RF','servicoModo(): poll WiFi\nFechar ciclo BLE+WiFi\nTimers sono / inatividade AP',1090,405,440,155,'radio')
p.node('panel','Resumo e fim','Painel 5 s / 30 s com AP\nSem resumo em download\nGuardar duração da volta',70,700,400,155)
p.node('retry','SD / retry pendente','Qualquer buffer cheio → pendente\nRetry ≥1 s; sem ciclo/scan/download\nflushBuffers()',590,700,400,155,'sd')
p.node('web','Eventos HTTP','imprimirAcessosWeb()\nimprimirDownload()\nImpressão só pela loopTask',1090,700,440,155,'http')
p.edge('start','focus','sim','warn');p.edge('start','acq','não','sensor');p.edge('acq','data');p.edge('data','mode');p.edge('mode','web');p.edge('web','retry');p.edge('retry','panel');p.edge('panel','start','próxima volta',side=(0,.5,0,.5),points=[(40,777),(40,255)])
p.note('note','Timers independem de fix novo, mas não executam no retorno de foco HTTP','Durante foco, não há leitura de sensores, novas linhas GPS, serviço de modos, retry SD ou impressão Serial.',590,900,940,80)

p=Page('05-maquina-estados','Máquina de estados movimento / parado','Histerese controla transições por RMC válido; servicoModo() controla tempo e fechamento dos ciclos.','esp32gpsd_v3.ino:764–949, 963–1011, 1459–1474')
p.node('mov','MOVIMENTO / 0','RMC válido dispara ciclo após ≥30 s\ndesde fim anterior. CSV: 10 s.\nWiFi STA + BLE quando escaneia.',90,230,560,160,'radio')
p.node('check','PARADO_CHECK / 3','Ao parar: reaproveitar ciclo aberto\nou iniciar ciclo final WiFi+BLE.\nAo acordar: iniciar ciclo único.',940,230,560,160,'radio')
p.node('sono','PARADO_SONO / 2','5 min sem scan • tentativa de flush\nSono lógico: sem deep sleep.\nAquisição continua fora do foco.',90,640,560,160)
p.node('hot','PARADO_HOTSPOT / 4','Flush → drenar fila → BLE deinit\nAP: 5 min sem ação HTTP válida\nCSV parado: 30 s, fora do foco.',940,640,560,160,'http')
p.edge('mov','check','km/h ≤2','radio');p.edge('check','hot','WiFi done + BLE done','http');p.edge('hot','sono','5 min inativo; sem download','http');p.edge('sono','check','5 min desde entrada','radio',side=(.5,0,.15,1),points=[(370,495),(1024,495)])
p.edge('check','sono','HTTP indisponível / AP falhou','warn',side=(0,.55,1,.3),points=[(830,318),(830,688)],dashed=True)
p.note('return','Qualquer estado parado → MOVIMENTO','>5 km/h por 5 RMCs válidos consecutivos; leitura ≤5 zera contador.\nAo sair do HOTSPOT: fechar AP; próximo scan reinicializa BLE.',70,855,790,110)
p.note('fresh','GPS e foco HTTP','Sem RMC válido, modo permanece; velocidade fica obsoleta em 3 s.\nNo foco HTTP, UART é descartada: movimento não é detectado.',900,855,630,110)
p.edge('sono','mov','>5 ×5 RMCs','sensor',side=(0,.5,0,.5),points=[(52,720),(52,310)],dashed=True)
p.label('resume','HOTSPOT/CHECK → MOVIMENTO seguem a mesma regra; download só aborta por movimento se a transição puder ser detectada.',90,170,1410,40,16)

p=Page('06-aquisicao','Aquisição GPS e sensores','Validade do NMEA, idade da velocidade e validação de calendário precedem gravação de telemetria.','esp32gpsd_v3.ino:512–524, 953–1067, 1478–1519')
p.lane('g','GPS / NMEA',48,175,485,660,'sensor');p.lane('i','SENSORES',558,175,485,660,'sensor');p.lane('c','COMPOSIÇÃO / CSV',1068,175,484,660,'sd')
p.node('parse','UART → receberGPS()','NMEA até 127 chars\ngps.encode(): checksum\nAceita $GPRMC / $GPGGA',75,245,430,135,'sensor')
p.node('rmc','RMC: velocidade / direção','Campos numéricos e posição\nknots ×1,852 → km/h\nVelocidade válida → atualizarModo()',75,435,430,150,'sensor')
p.node('gga','GGA: satélites / HDOP','Atualiza leitura de posição\nNão renova velocidade RMC\nSem velocidade nova: manter modo',75,635,430,145,'sensor')
p.node('mpu','MPU6050','Amostra a cada 100 ms\nMédia por janela de ≈1 s\nSem MPU: campos IMU vazios',585,245,430,135,'sensor')
p.node('dht','DHT22','Intervalo mínimo 2 s\nNaN → campo CSV vazio\nÚltima leitura alimenta página',585,435,430,150,'sensor')
p.node('units','Unidades / freshness','ac: m/s² • gy: rad/s\nVelocidade CSV só se idade <3 s\nSensores pausam no foco HTTP',585,635,430,145,'warn')
p.node('valid','processarDadosGPS()','Atualizar última posição válida\nValidar data/hora/calendário\nNormalizar UTC−3 com mktime',1095,245,430,155,'sd')
p.node('csv','Compor 15 colunas','Posição em milionésimos de grau\nCampos inválidos vazios\nLinha GPS: máximo 159 chars',1095,475,430,135,'sd')
p.node('cad','Cadência → logBuffer','MOVIMENTO: ≥10 s\nQualquer PARADO: ≥30 s\nSomente quando há newData',1095,690,430,115,'sd')
p.edge('parse','rmc','RMC','sensor');p.edge('parse','gga','GGA','sensor',side=(0,.6,0,.5),points=[(60,326),(60,707)]);p.edge('rmc','valid','leituraGPS / modo','sensor',side=(1,.25,0,.95),points=[(542,472),(542,412),(1050,412),(1050,392)]);p.edge('mpu','csv','média IMU','sensor',side=(1,.4,0,.3));p.edge('dht','csv','DHT','sensor');p.edge('valid','csv','data/hora válidas','sd');p.edge('csv','cad','intervalo atingido','sd')
p.note('pos','Posição dos achados de rádio','WiFi/BLE usam lastLat / lastLon / lastTimeStamp na composição; sem fix inicial, coordenadas vazias.\nHora pode permanecer antiga: não é posição exata no instante da detecção.',75,875,1450,100)

p=Page('07-ciclo-radio','Ciclo coordenado WiFi + BLE','Ciclo só encerra após ambos concluírem ou sinalizarem falha; durações não são iguais.','esp32gpsd_v3.ino:555–619, 671–807, 882–949')
p.node('trigger','iniciarCicloRadio()','Reset flags / stats • cicloNumero++\nBLE começa aqui; WiFi no serviço\nradioCicloAtivo=true',565,185,470,130,'radio')
p.lane('w','RAMO WIFI / loopTask',60,360,680,440,'radio');p.lane('b','RAMO BLE / PILHA + loopTask',860,360,680,440,'radio')
p.node('wifi','varrerWiFi()','scanNetworks(true, true)\nAssíncrono; incluir SSIDs ocultos\nPoll scanComplete() no serviço',90,425,620,135,'radio')
p.node('wdone','WiFi finalizado','Resultado ≥0 → dedup + wifiBuffer\nscanDelete(); scanEmAndamento=false\nFalha → flag; wifiDone=true',90,620,620,135,'radio')
p.node('ble','bleScanLigar()','Reiniciar NimBLE se necessário\nScan ativo: 5000 ms\nCallbacks não reiniciam scan',890,425,620,135,'radio')
p.node('bdone','BLE finalizado','onScanEnd → flags atômicas\nloopTask drena fila; bleDone=true\nFalha start → bleDone=true',890,620,620,135,'radio')
p.node('join','atualizarCicloRadio() / junção','wifiDone && bleDone → fechar ciclo; guardar ultimoCicloFim.\nEm PARADO_CHECK → entrarModoParadoHotspot(); em movimento → aguardar novo RMC e ≥30 s.',310,855,980,115,'base')
p.edge('trigger','wifi','servicoModo()','radio',side=(.15,1,.5,0),points=[(635,340),(400,340)]);p.edge('trigger','ble','início BLE','radio',side=(.85,1,.5,0),points=[(965,340),(1200,340)]);p.edge('wifi','wdone','running → seguir poll; concluído/falhou','radio');p.edge('ble','bdone','fim / falha de início','radio');p.edge('wdone','join','WiFi done','radio');p.edge('bdone','join','BLE done','radio')

p=Page('08-deduplicacao','Fila BLE e deduplicação por hash','Callback produz registros sem bloquear; loopTask é dona da formatação, caches e buffers.','esp32gpsd_v3.ino:286–311, 622–706, 579–611')
p.node('cb','NimBLE onResult()','malloc BLEDeviceRecord\nMAC / nome até 64 / RSSI / TX\nFalha malloc → perdas++',65,195,440,145,'radio')
p.node('q','FreeRTOS bleQueue','10 ponteiros • xQueueSend(...,0)\nFila cheia → free + perdas++\nFiltro nativo reduz duplicatas',580,195,440,145,'radio')
p.node('drain','drenarFilaBLE()','xQueueReceive(...,0)\ndashBle++ para cada consumido\nAo final: free(rec)',1095,195,440,145,'radio')
p.node('key','Identificador observado','WiFi: SSID, não BSSID\nBLE: endereço MAC\nCalcula FNV-1a 32 bits',1095,455,440,145)
p.node('cache','Hash já visto?','Busca linear em cache RTC\nSSID 500 / MAC BLE 500\nConhecido → ignorar CSV',580,455,440,145)
p.node('buf','Novo → cache + CSV','Adiciona hash antes do flush\nCarimba última posição/hora\nBuffer circular WiFi ou BLE',65,455,440,145,'sd')
p.edge('cb','q','ponteiro','radio');p.edge('q','drain','consumo no loop','radio');p.edge('drain','key','MAC','radio');p.edge('key','cache','hash');p.edge('cache','buf','não visto','sd')
p.note('full','Cache cheia','Aviso único. Conhecidos continuam filtrados.\nNovos IDs geram CSV, mas não entram no cache.',65,735,440,165)
p.note('rtc','Retenção RTC','Caches e bootCount usam RTC_DATA_ATTR.\nIntenção: reter em resets, não power-off.\nRetenção real depende do tipo de reset.',580,735,440,165)
p.note('limit','Limites de deduplicação','Hash 32 bits pode colidir. SSID igual agrupa redes.\nHash é inserido antes de escrita confirmada: perda\nde buffer não garante reobservação do registro.',1095,735,440,165)

p=Page('09-armazenamento','Buffers, flush e recuperação do SD','Confirmação por arquivo/lote; não existe transação única envolvendo os três arquivos.','esp32gpsd_v3.ino:313–472, 831–878, 1536–1542')
p.node('ram','Acumular em RAM','log: 150×160 B • WiFi/BLE: 50×256 B\nTotal: 49.600 B de buffers\nCheio → sobrescrever mais antiga + perdas',60,190,460,150,'sd')
p.node('req','Solicitar flush','Qualquer buffer cheio ou\nentrada HOTSPOT / SONO\nflushPendente=true',570,190,460,150,'sd')
p.node('gate','Pode gravar agora?','Sem scan WiFi/BLE e download\nAdquirir sdMutex; checar download\nSe bloqueado: manter pendente',1080,190,460,150,'warn')
p.node('write','appendLinhasCirculares()','log → wifi → ble; um open/close por lote\nO_APPEND; ordem cronológica\nSem cópia intermediária do lote',1080,440,460,150,'sd')
p.node('ok','Escrita + sync + close OK?','Exigir tamanho exato por linha\nSucesso → remover lote daquele buffer\nFalha → reter lote inteiro',570,440,460,150,'sd')
p.node('success','Todos os lotes confirmados','Liberar mutex\nflushPendente=false\nEvento [OK ]',60,440,460,150,'sd')
p.node('remount','Falha: remontar cartão','Ainda sob sdMutex: sd.end()\n50 ms → sd.begin()\nSucesso zera falhasRemountSD',570,735,460,150,'warn')
p.node('reset','10 remounts falhos seguidos','esp_restart()\nBoot volta ao estado MOVIMENTO\nRAM não gravada pode se perder',1080,735,460,150,'warn')
p.node('retry','Retry no loop ≥1 s','Manter pendente até confirmar\nSem ciclo/scan/download ativo\nRetry pode duplicar escrita parcial',60,735,460,150,'warn')
p.edge('ram','req');p.edge('req','gate');p.edge('gate','write','permitido','sd');p.edge('write','ok');p.edge('ok','success','todos OK','sd');p.edge('ok','remount','algum falhou','warn');p.edge('remount','reset','limite atingido','warn');p.edge('remount','retry','abaixo do limite','warn');p.edge('retry','req','próxima tentativa','warn',side=(0,.5,.5,1),points=[(40,810),(40,650),(800,650)])
p.label('risk','Retenção do lote incerto preserva dados para retry; energia interrompida perde RAM. Flush falho não impede tentativa de abrir AP.',60,925,1480,45,17)

p=Page('10-concorrencia','Tarefas, ownership e exclusão mútua','loopTask e Hotspot_HTTP compartilham core 1; pilhas do core/IDF atendem rádio em seus contextos.','esp32gpsd_v3.ino:95–146, 643–706, 1229–1341, 1467–1547')
p.lane('core0','PILHAS DE RÁDIO / CONTEXTO IDF',48,175,440,760,'radio');p.lane('core1','CORE 1 / APLICAÇÃO',518,175,635,760);p.lane('shared','RECURSOS COMPARTILHADOS',1183,175,369,760,'sd')
p.node('stack','WiFi + NimBLE','Execução interna do core/IDF\nCallback BLE: sem Serial / SD\nAfinidade interna não definida aqui',73,245,390,170,'radio')
p.node('queue','Fila / flags BLE','10 ponteiros → loopTask\nbleFimPendente / bleScanAtivo\nFalhas incrementam perdidos',73,610,390,150,'radio')
p.node('loop','loopTask','Sensores / GPS / modos\nDona dos buffers e caches\nÚnica emissora de Serial\nEscrita SD + retry / remount',548,245,575,210)
p.node('http','Hotspot_HTTP','Prioridade 2 • stack 8192 B\nserver.handleClient()\nTick 2 ms aberto / 100 ms fechado\nLê SD; publica métricas atômicas',548,610,575,210,'http')
p.node('sd','sdMutex / SdFs','Flush/remount no loop\nHTTP: listagem e leitura\nSerializa operações SD',1208,245,319,170,'sd')
p.node('state','Estado de download','downloadAtivo sob mutex\nImpede flush/remount\nArquivo aberto entre blocos',1208,500,319,145,'http')
p.node('dash','Snapshots / métricas','DHT / scans: volatile\nEstado HTTP: atomic\nPágina usa últimos valores',1208,750,319,145,'base')
p.edge('stack','queue','onResult / onScanEnd','radio');p.edge('queue','loop','drenarFilaBLE()','radio',side=(1,.2,0,.8));p.edge('loop','sd','lock / escrita','sd');p.edge('http','state','lock / flag','http',side=(1,.2,0,.6),points=[(1158,652),(1158,587)]);p.edge('state','sd','guard de arquivo aberto','sd');p.edge('http','dash','métricas HTTP','http',side=(1,.85,0,.75),points=[(1163,788),(1163,859)]);p.edge('loop','dash','últimas leituras',side=(1,.9,0,.15),points=[(1180,434),(1180,772)],dashed=True)
p.label('n','Não há BLE_Consumer na v3. Buffers não precisam de mutex próprio; HTTP não os modifica.',73,945,1450,36,17)

p=Page('11-hotspot','Hotspot, página e foco HTTP','AP local sem internet; dados exibidos são snapshots e atualização ocorre por recarga manual.','esp32gpsd_v3.ino:810–911, 1110–1225, 1459–1475')
p.node('pre','Fim do CHECK','Sem ciclo / scan WiFi / BLE\nTentativa de flush → drenar fila\nbleParar(): deinit libera heap',60,190,460,150,'radio')
p.node('ap','Abrir AP','WIFI_AP • 192.168.4.1/24\nESP32GPS-Logs • canal 1\nMáximo 1 estação • senha configurada',570,190,460,150,'http')
p.node('root','GET / • httpRaiz()','HTML5 + CSS embutido\nSem JS / recursos externos / API\nResposta chunked • Cache-Control no-store',1080,190,460,150,'http')
p.node('cards','Painel e arquivos','WiFi / BLE últimos scans\nDHT última leitura; NaN → sem leitura\nListar nomes / tamanhos via sdMutex',1080,440,460,150,'http')
p.node('act','Atividade válida','Abrir / atualizar página ou baixar\nrenova ultimaAtividade\nSó conectar no AP não renova',570,440,460,150,'http')
p.node('focus','Prioridade HTTP','Acesso recente <8 s ou download\n→ UART descartada; WDT + delay\nGPS / MPU / DHT / SD / modo pausam',60,440,460,150,'warn')
p.node('wait','Após foco: retomar loop','Volta aquisição / timers / Serial\nSem atividade por 5 min e\nsem download → fechar AP',60,735,460,150,'http')
p.node('sleep','PARADO_SONO / 5 min','Tentativa de flush\nFim do intervalo → CHECK\nPróximo scan religa NimBLE',570,735,460,150)
p.node('fail','Falha AP ou tarefa HTTP','AP falhou → WIFI_STA\nSem tarefa → hotspot desabilitado\nCHECK vai ao sono; logger continua',1080,735,460,150,'warn')
p.edge('pre','ap');p.edge('ap','root','navegador','http');p.edge('root','cards');p.edge('cards','act','página / download','http');p.edge('act','focus');p.edge('focus','wait','sem foco');p.edge('wait','sleep','inativo');p.edge('ap','fail','softAP falhou','warn',side=(.9,1,.5,0),points=[(1052,385),(1052,665),(1310,665)],dashed=True)
p.label('n','BLE não permanece ligado no hotspot. Sem foco HTTP, aquisição normal continua; dados não são coletados durante download.',60,925,1480,45,17)

p=Page('12-download','Download HTTP: validação, streaming e encerramento','GET /download?file=... envia bytes originais do SD; não converte CSV nem oferece escrita/upload.','esp32gpsd_v3.ino:276–284, 1110–1115, 1219–1322')
p.node('req','Receber requisição','DownloadEstado != OCIOSO → 503\nAllowlist: log.txt / wifi.txt / ble.txt\nNome fora da lista → 400',60,190,460,150,'http')
p.node('open','Validar sob sdMutex','Lock até 2 s → ocupado: 503\nVolume/off/open falhou: 503\nArquivo ausente: 404',570,190,460,150,'sd')
p.node('header','Abrir e enviar headers','Tamanho ≤4.294.967.295 B\nMaior → 503; downloadAtivo=true\nContent-Length / attachment / no-store',1080,190,460,150,'http')
p.node('read','Ler bloco do arquivo','Blocos de até 2048 B\nsdMutex só durante f.read()\nLeitura curta → abortar',1080,445,460,150,'sd')
p.node('send','Enviar via NetworkClient','client.write() pode ser parcial\nAtualizar bytes / último progresso\nSem escrita → stalls++; delay 2 ms',570,445,460,150,'http')
p.node('abort','Condição de aborto','AP fechado ou desconexão\n15 s sem progresso\nErro leitura SD',60,445,460,150,'warn')
p.node('close','Encerrar sob sdMutex','f.close() • downloadAtivo=false\nclient.stop()\nRenovar ultimaAtividade',60,745,460,150,'sd')
p.node('result','Resultado atômico','Concluído: sem motivo, close OK\ne bytes enviados = total\nCaso contrário: DOWNLOAD_ERRO',570,745,460,150,'http')
p.node('print','loopTask imprime transição','[OK ] / [ERR]: bytes / duração\nMotivo / stalls / heap / maxblk\nLiberar estado para novo download',1080,745,460,150)
p.edge('req','open','nome permitido');p.edge('open','header','SD / arquivo OK');p.edge('header','read');p.edge('read','send','bytes lidos');p.edge('send','read','próximo bloco','http',side=(.5,0,.5,0),points=[(800,400),(1310,400)]);p.edge('send','abort','verificar entre writes','warn');p.edge('abort','close','abortou','warn');p.edge('send','close','total enviado','sd',side=(.5,1,.5,0),points=[(800,680),(290,680)]);p.edge('close','result');p.edge('result','print')
p.label('n','Enquanto arquivo está aberto: sem flush/remount. Download incompleto fecha conexão antes de Content-Length → navegador detecta falha.',60,940,1480,36,16)

p=Page('13-dados-csv','Contratos de dados e memória','Três arquivos locais; dados numéricos com unidades explícitas e timestamp normalizado em UTC−3.','esp32gpsd_v3.ino:162–199, 474–481, 595–609, 651–664, 1018–1067')
p.node('log','/log.txt • GPS + sensores','Cabeçalho de 15 colunas:\ndata_hora, lat, lon, sat, hdop, kmh, direcao,\numidade, temp_dht, ac_x, ac_y, ac_z,\ngy_x, gy_y, gy_z\nMarcador: # BOOT bootCount=N reset_reason=R',65,215,700,220,'sd')
p.node('wifi','/wifi.txt • achados WiFi','Sem cabeçalho • 7 colunas:\ndata_hora, lat, lon, SSID, RSSI, canal, criptografia\nDedup por hash do SSID\nRSSI: dBm • segurança em texto',835,215,700,220,'radio')
p.node('ble','/ble.txt • achados BLE','Sem cabeçalho • 7 colunas:\ndata_hora, lat, lon, MAC, nome, RSSI, tx_power\nNome / TX vazios se não anunciados\nPosição/hora no consumo da fila, não no callback',65,505,700,200,'radio')
p.node('units','Representação / exportação','DD/MM/AAAA HH:MM:SS • UTC−3 fixo\nlat/lon ÷1.000.000 → graus decimais\nkm/h; umidade %; DHT °C; ac m/s²; gy rad/s\nDownloads preservam bytes exatos do arquivo',835,505,700,200,'sensor')
p.note('heap','RAM / heap volátil','logBuffer: 24.000 B • WiFi: 12.800 B • BLE: 12.800 B.\nFila BLE: 10 ponteiros + records alocados dinamicamente.\nPower-off perde dados não gravados.',65,795,700,160,'warn')
p.note('rtc','RTC / flash / SD','RTC: 2 caches ×500 hashes ×4 B + contadores de cache/boot.\nFlash: firmware + PAGINA_CSS; não grava telemetria em SPIFFS.\nSD: histórico CSV; não há rotação implementada.',835,795,700,160,'base')

p=Page('14-build','Dependências e entrega do firmware','Bibliotecas do projeto são fonte principal; sincronizar e verificar antes de compilar nova versão.','../libraries/manifest.json; ../CONTEXT.md; ../docs/wiki/compilacao-arduino-cli.md')
p.node('lib','libraries/ • versões registradas','TinyGPS 13.0.0 • SdFat 2.3.0 • DHT 1.4.7\nMPU6050 2.2.9 • Unified Sensor 1.1.15\nBusIO 1.17.4 • NimBLE-Arduino 2.5.0',65,200,700,175)
p.node('core','ESP32 Arduino Core 3.3.12','WiFi / WebServer / NetworkClient\nSPI / Wire / FreeRTOS / esp_task_wdt\nPlaca: esp32:esp32:esp32',835,200,700,175)
p.node('sync','Sincronizar projeto → IDE','Na raiz esp32/:\npython3 libraries/sync.py --apply\npython3 libraries/sync.py --check\nDivergência impede considerar build validado',65,450,700,175)
p.node('compile','Compilar v3 / no_ota','arduino-cli compile\n--fqbn esp32:esp32:esp32:PartitionScheme=no_ota\nesp32gpsd_v3\nPartição APP: 2.097.152 B; sem OTA',835,450,700,175)
p.node('upload','Gravar / monitorar','Upload na porta correta com mesmo FQBN\nPorta documentada: /dev/ttyACM0\nConsole Serial: 115200 baud\nPorta precisa estar livre durante upload',835,735,700,175,'sensor')
p.node('scope-build','Uso real das dependências','GFX / SSD1306 no manifesto, mas fora da v3.\nWiFi + NimBLE exigem APP maior que padrão.\nNão há display OLED, SIM800L ou backend nesta v3.',65,735,700,175,'warn')
p.edge('lib','sync');p.edge('core','compile');p.edge('sync','compile','check OK');p.edge('compile','upload','binário validado')

p=Page('15-diagnostico','Observabilidade e limites operacionais','Console concentra diagnóstico; estados HTTP e callbacks publicam métricas para a loopTask.','esp32gpsd_v3.ino:246–284, 525–552, 1069–1108; serial.md; tests/test_serial.py')
p.node('events','Eventos imediatos','[EVT] / [ERR] / [OK ]\nUptime + número/fase do loop\nBoot, rádio, SD e download',65,200,700,160)
p.node('panel','Resumo Serial','5 s normal / 30 s com AP\nSem resumo durante download\nModo / GPS / DHT / az / RF / buffers',835,200,700,160)
p.node('metrics','Métricas operacionais','SD confirmado/incerto • perdidos\nHeap livre + maior bloco\nClientes AP / páginas / downloads\nÚltimo download: motivo, stalls, bytes, duração',65,435,700,175)
p.node('recover','Recuperação automática','Watchdog loopTask: 60 s\nFalha SD: retry / remount; 10 falhas → reset\nFalha AP: ir ao sono; próximo check tenta\nMPU ausente: continuar sem IMU',835,435,700,175,'warn')
p.node('host','Regressão no host','tests/test_serial.py • TinyGPS real / SD simulado\nChecksum, RMC/GGA, idade e UTC−3\nEscrita parcial / sync / close\nCache cheia / sobrescrita circular',65,735,700,200,'sd')
p.node('field','Verificação em placa / pendente nas docs','GPS ausente • transições de modo\nHotspot/download e remoção do SD\nCaptura Serial com versão do firmware\nFoco HTTP impede detecção de movimento\nSem aquisição ou novas linhas GPS no foco',835,735,700,200,'warn')
p.edge('events','metrics');p.edge('panel','recover');p.edge('metrics','host','validar comportamento',dashed=True);p.edge('recover','field','validar comportamento físico','warn',dashed=True)

root=E.Element('mxfile',host='app.diagrams.net',agent='ESP32 v3 documentation',version='31.4.5',type='device',compressed='false')
for p in PAGES:
    cells=p.diagram.findall('.//mxCell')
    ids=[c.get('id') for c in cells]
    assert len(ids)==len(set(ids)), f'IDs duplicados: {p.slug}'
    for c in cells:
        if c.get('edge')=='1':
            assert c.get('source') in ids and c.get('target') in ids
    root.append(p.diagram)
E.indent(root)
E.ElementTree(root).write(OUT/'esp32gpsd-v3.drawio',encoding='utf-8',xml_declaration=True)
(OUT/'paginas.tsv').write_text(''.join(f'{i}\t{p.slug}\n' for i,p in enumerate(PAGES,1)))
print(f'{len(PAGES)} páginas DrawIO geradas.')
