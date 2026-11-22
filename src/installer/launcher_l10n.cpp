#include "launcher_l10n.h"

#include <windows.h>

#include <cstring>

namespace edvr::installer {
namespace {

struct Row {
    const wchar_t* en;
    const wchar_t* de;
    const wchar_t* es;
    const wchar_t* fr;
    const wchar_t* pt;
    const wchar_t* ru;
};

const Row kRows[] = {
    {L"Setup", L"Einrichtung", L"Instalaci\u00f3n", L"Installation", L"Instala\u00e7\u00e3o", L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430"},
    {L"Settings", L"Einstellungen", L"Ajustes", L"R\u00e9glages", L"Configura\u00e7\u00f5es", L"\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438"},
    {L"Browse...", L"Durchsuchen...", L"Examinar...", L"Parcourir...", L"Procurar...", L"\u041e\u0431\u0437\u043e\u0440..."},
    {L"Keep the settings I have changed", L"Meine ge\u00e4nderten Einstellungen behalten", L"Conservar los ajustes que he cambiado", L"Garder les r\u00e9glages que j'ai chang\u00e9s", L"Manter as configura\u00e7\u00f5es que alterei", L"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c \u043c\u043e\u0438 \u0438\u0437\u043c\u0435\u043d\u0435\u043d\u0438\u044f"},
    {L"Install", L"Installieren", L"Instalar", L"Installer", L"Instalar", L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c"},
    {L"Repair", L"Reparieren", L"Reparar", L"R\u00e9parer", L"Reparar", L"\u0412\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c"},
    {L"Uninstall", L"Entfernen", L"Desinstalar", L"D\u00e9sinstaller", L"Desinstalar", L"\u0423\u0434\u0430\u043b\u0438\u0442\u044c"},
    {L"Save logs", L"Protokolle speichern", L"Guardar registros", L"Enregistrer les journaux", L"Salvar registros", L"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c \u0436\u0443\u0440\u043d\u0430\u043b\u044b"},
    {L"VR and flat (recommended)", L"VR und flach (empfohlen)", L"RV y plano (recomendado)", L"RV et plat (recommand\u00e9)", L"RV e plano (recomendado)", L"VR \u0438 \u0431\u0435\u0437 VR (\u0440\u0435\u043a\u043e\u043c\u0435\u043d\u0434\u0443\u0435\u0442\u0441\u044f)"},
    {L"VR only", L"Nur VR", L"Solo RV", L"RV seulement", L"Somente RV", L"\u0422\u043e\u043b\u044c\u043a\u043e VR"},
    {L"Flat only", L"Nur flach", L"Solo plano", L"Plat seulement", L"Somente plano", L"\u0422\u043e\u043b\u044c\u043a\u043e \u0431\u0435\u0437 VR"},
    {L"VR", L"VR", L"RV", L"RV", L"RV", L"VR"},
    {L"Flat", L"Flach", L"Plano", L"Plat", L"Plano", L"\u0411\u0435\u0437 VR"},
    {L"Download & install", L"Herunterladen & installieren", L"Descargar e instalar", L"T\u00e9l\u00e9charger et installer", L"Baixar e instalar", L"\u0421\u043a\u0430\u0447\u0430\u0442\u044c \u0438 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c"},
    {L"Download & update", L"Herunterladen & aktualisieren", L"Descargar y actualizar", L"T\u00e9l\u00e9charger et mettre \u00e0 jour", L"Baixar e atualizar", L"\u0421\u043a\u0430\u0447\u0430\u0442\u044c \u0438 \u043e\u0431\u043d\u043e\u0432\u0438\u0442\u044c"},
    {L"Include release candidates", L"Release-Kandidaten einschlie\u00dfen", L"Incluir versiones candidatas", L"Inclure les versions candidates", L"Incluir vers\u00f5es candidatas", L"\u0412\u043a\u043b\u044e\u0447\u0430\u0442\u044c \u043a\u0430\u043d\u0434\u0438\u0434\u0430\u0442\u044b"},
    {L"Play", L"Spielen", L"Jugar", L"Jouer", L"Jogar", L"\u0418\u0433\u0440\u0430\u0442\u044c"},
    {L"Play VR", L"VR spielen", L"Jugar en RV", L"Jouer en RV", L"Jogar em RV", L"\u0418\u0433\u0440\u0430\u0442\u044c \u0432 VR"},
    {L"Play flat", L"Flach spielen", L"Jugar en plano", L"Jouer en plat", L"Jogar em plano", L"\u0418\u0433\u0440\u0430\u0442\u044c \u0431\u0435\u0437 VR"},
    {L"Start Elite directly (no Frontier launcher)", L"Elite direkt starten (ohne Frontiers Launcher)", L"Iniciar Elite directamente (sin el launcher de Frontier)", L"Lancer Elite directement (sans le launcher Frontier)", L"Iniciar Elite diretamente (sem o launcher da Frontier)", L"\u0417\u0430\u043f\u0443\u0441\u043a\u0430\u0442\u044c Elite \u043d\u0430\u043f\u0440\u044f\u043c\u0443\u044e (\u0431\u0435\u0437 \u043b\u0430\u0443\u043d\u0447\u0435\u0440\u0430 Frontier)"},
    {L"Use Frontier account login", L"Frontier-Konto zur Anmeldung nutzen", L"Usar inicio de sesi\u00f3n de cuenta Frontier", L"Utiliser la connexion par compte Frontier", L"Usar login de conta Frontier", L"\u0412\u0445\u043e\u0434\u0438\u0442\u044c \u0447\u0435\u0440\u0435\u0437 \u0430\u043a\u043a\u0430\u0443\u043d\u0442 Frontier"},
    {L"Close the launcher once Elite has started", L"Den Launcher schlie\u00dfen, sobald Elite l\u00e4uft", L"Cerrar el launcher cuando Elite haya arrancado", L"Fermer le launcher une fois Elite lanc\u00e9", L"Fechar o launcher quando Elite tiver iniciado", L"\u0417\u0430\u043a\u0440\u044b\u0442\u044c \u043b\u0430\u0443\u043d\u0447\u0435\u0440, \u043a\u043e\u0433\u0434\u0430 Elite \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u0441\u044f"},
    {L"Clean purge on uninstall", L"Vollst\u00e4ndig bereinigen beim Entfernen", L"Limpieza completa al desinstalar", L"Nettoyage complet \u00e0 la d\u00e9sinstallation", L"Limpeza completa ao desinstalar", L"\u041f\u043e\u043b\u043d\u0430\u044f \u043e\u0447\u0438\u0441\u0442\u043a\u0430 \u043f\u0440\u0438 \u0443\u0434\u0430\u043b\u0435\u043d\u0438\u0438"},
    {L"Clean purge permanently deletes ED-UP settings, logs, install records, the user settings folder, and everything inside edup_backup\\ (including any backups of other mods' files or earlier edup.ini copies).",
     L"Die vollst\u00e4ndige Bereinigung l\u00f6scht ED-UP-Einstellungen, Protokolle, Installationsdaten, den Benutzereinstellungsordner und alles in edup_backup\\ dauerhaft (einschlie\u00dflich Sicherungen anderer Mods und fr\u00fcherer edup.ini-Dateien).",
     L"La limpieza completa elimina de forma permanente los ajustes de ED-UP, registros, datos de instalaci\u00f3n, la carpeta de ajustes de usuario y todo el contenido de edup_backup\\ (incluidas copias de seguridad de otros mods o archivos edup.ini anteriores).",
     L"Le nettoyage complet supprime d\u00e9finitivement les r\u00e9glages d'ED-UP, les journaux, les fichiers d'installation, le dossier des r\u00e9glages utilisateur et tout le contenu d'edup_backup\\ (y compris les sauvegardes d'autres mods ou d'anciens edup.ini).",
     L"A limpeza completa apaga permanentemente as configura\u00e7\u00f5es do ED-UP, registros, dados de instala\u00e7\u00e3o, a pasta de configura\u00e7\u00f5es do usu\u00e1rio e tudo dentro de edup_backup\\ (incluindo backups de outros mods ou c\u00f3pias antigas de edup.ini).",
     L"\u041f\u043e\u043b\u043d\u0430\u044f \u043e\u0447\u0438\u0441\u0442\u043a\u0430 \u0431\u0435\u0437\u0432\u043e\u0437\u0432\u0440\u0430\u0442\u043d\u043e \u0443\u0434\u0430\u043b\u0438\u0442 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 ED-UP, \u0436\u0443\u0440\u043d\u0430\u043b\u044b, \u0437\u0430\u043f\u0438\u0441\u0438 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438, \u043f\u0430\u043f\u043a\u0443 \u043d\u0430\u0441\u0442\u0440\u043e\u0435\u043a \u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f \u0438 \u0432\u0441\u0451 \u0432 edup_backup\\ (\u0432\u043a\u043b\u044e\u0447\u0430\u044f \u0440\u0435\u0437\u0435\u0440\u0432\u043d\u044b\u0435 \u043a\u043e\u043f\u0438\u0438 \u0434\u0440\u0443\u0433\u0438\u0445 \u043c\u043e\u0434\u043e\u0432 \u0438 \u043f\u0440\u0435\u0436\u043d\u0438\u0435 edup.ini)."},
    {L"Status", L"Stand", L"Estado", L"\u00c9tat", L"Estado", L"\u0421\u043e\u0441\u0442\u043e\u044f\u043d\u0438\u0435"},
    {L"Updates", L"Aktualisierungen", L"Actualizaciones", L"Mises \u00e0 jour", L"Atualiza\u00e7\u00f5es", L"\u041e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f"},
    {L"Install & repair", L"Installieren & reparieren", L"Instalar y reparar", L"Installer et r\u00e9parer", L"Instalar e reparar", L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u0438 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0435"},
    {L"Log", L"Protokoll", L"Registro", L"Journal", L"Registro", L"\u0416\u0443\u0440\u043d\u0430\u043b"},
    {L"Launch", L"Starten", L"Iniciar", L"Lancement", L"Iniciar", L"\u0417\u0430\u043f\u0443\u0441\u043a"},
    {L"Language", L"Sprache", L"Idioma", L"Langue", L"Idioma", L"\u042f\u0437\u044b\u043a"},
    {L"Install ED-UP for:", L"ED-UP installieren f\u00fcr:", L"Instalar ED-UP para:", L"Installer ED-UP pour :", L"Instalar o ED-UP para:", L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c ED-UP \u0434\u043b\u044f:"},
    {L"The fixes", L"Die Korrekturen", L"Las correcciones", L"Les correctifs", L"As corre\u00e7\u00f5es", L"\u0418\u0441\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u044f"},
    {L"not installed", L"nicht installiert", L"no instalado", L"non install\u00e9", L"n\u00e3o instalado", L"\u043d\u0435 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u043e"},
    {L"ED-UP installed as d3d11.dll", L"ED-UP ist als d3d11.dll installiert", L"ED-UP instalado como d3d11.dll", L"ED-UP install\u00e9 en tant que d3d11.dll", L"ED-UP instalado como d3d11.dll", L"ED-UP \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d \u043a\u0430\u043a d3d11.dll"},
    {L"Flash fix, native on foot", L"Blitz-Korrektur, zu Fu\u00df nativ", L"Correcci\u00f3n del destello, a pie nativo", L"Correctif d'\u00e9clair, \u00e0 pied natif", L"Corre\u00e7\u00e3o do clar\u00e3o, a p\u00e9 nativo", L"\u0418\u0441\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u0435 \u0432\u0441\u043f\u044b\u0448\u043a\u0438, \u043f\u0435\u0448\u043a\u043e\u043c"},
    {L"Elite's 3D mode is unknown: ED-UP starts in VR.", L"Elites 3D-Modus ist unbekannt: ED-UP startet in VR.", L"El modo 3D de Elite es desconocido: ED-UP arranca en RV.", L"Le mode 3D d'Elite est inconnu : ED-UP d\u00e9marre en RV.", L"O modo 3D do Elite \u00e9 desconhecido: o ED-UP inicia em RV.", L"\u0420\u0435\u0436\u0438\u043c 3D Elite \u043d\u0435\u0438\u0437\u0432\u0435\u0441\u0442\u0435\u043d: ED-UP \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442\u0441\u044f \u0432 VR."},
    {L"Elite's 3D mode is unset: flat install runs its flat fixes.", L"Elites 3D-Modus ist nicht gesetzt: flache Installation nutzt die flachen Korrekturen.", L"El modo 3D de Elite no est\u00e1 definido: la instalaci\u00f3n plana usa sus correcciones planas.", L"Le mode 3D d'Elite n'est pas d\u00e9fini : l'installation plate applique ses correctifs plats.", L"O modo 3D do Elite n\u00e3o est\u00e1 definido: a instala\u00e7\u00e3o plana usa suas corre\u00e7\u00f5es planas.", L"\u0420\u0435\u0436\u0438\u043c 3D Elite \u043d\u0435 \u0437\u0430\u0434\u0430\u043d: \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u0431\u0435\u0437 VR \u0432\u043a\u043b\u044e\u0447\u0430\u0435\u0442 \u0438\u0441\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u044f \u0431\u0435\u0437 VR."},
    {L"Elite's 3D mode is off: ED-UP runs its flat fixes.", L"Elites 3D-Modus ist aus: ED-UP nutzt die flachen Korrekturen.", L"El modo 3D de Elite est\u00e1 apagado: ED-UP usa las correcciones planas.", L"Le mode 3D d'Elite est d\u00e9sactiv\u00e9 : ED-UP applique les correctifs plats.", L"O modo 3D do Elite est\u00e1 desligado: o ED-UP usa as corre\u00e7\u00f5es planas.", L"\u0420\u0435\u0436\u0438\u043c 3D Elite \u0432\u044b\u043a\u043b\u044e\u0447\u0435\u043d: ED-UP \u0432\u043a\u043b\u044e\u0447\u0430\u0435\u0442 \u0438\u0441\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u044f \u0431\u0435\u0437 VR."},
    {L"Elite's 3D mode is on: ED-UP runs its VR fixes.", L"Elites 3D-Modus ist an: ED-UP nutzt die VR-Korrekturen.", L"El modo 3D de Elite est\u00e1 encendido: ED-UP usa las correcciones de RV.", L"Le mode 3D d'Elite est activ\u00e9 : ED-UP applique les correctifs RV.", L"O modo 3D do Elite est\u00e1 ligado: o ED-UP usa as corre\u00e7\u00f5es de RV.", L"\u0420\u0435\u0436\u0438\u043c 3D Elite \u0432\u043a\u043b\u044e\u0447\u0451\u043d: ED-UP \u0432\u043a\u043b\u044e\u0447\u0430\u0435\u0442 \u0438\u0441\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u044f VR."},
    {L"This install does not include flat: ED-UP stays out of the way.", L"Diese Installation enth\u00e4lt kein Flach: ED-UP bleibt au\u00dfen vor.", L"Esta instalaci\u00f3n no incluye el modo plano: ED-UP no interviene.", L"Cette installation n'inclut pas le plat : ED-UP reste \u00e0 l'\u00e9cart.", L"Esta instala\u00e7\u00e3o n\u00e3o inclui o modo plano: o ED-UP fica de fora.", L"\u042d\u0442\u0430 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u0431\u0435\u0437 \u0440\u0435\u0436\u0438\u043c\u0430 \u0431\u0435\u0437 VR: ED-UP \u043d\u0435 \u0432\u043c\u0435\u0448\u0438\u0432\u0430\u0435\u0442\u0441\u044f."},
    {L"This install does not include VR: turn 3D off in Elite or use Play flat.", L"Diese Installation enth\u00e4lt kein VR: Schalte 3D in Elite aus oder nutze Flach spielen.", L"Esta instalaci\u00f3n no incluye RV: desactiva el 3D en Elite o usa Jugar en plano.", L"Cette installation n'inclut pas la RV : d\u00e9sactive la 3D dans Elite ou utilise Jouer en plat.", L"Esta instala\u00e7\u00e3o n\u00e3o inclui RV: desligue o 3D no Elite ou use Jogar em plano.", L"\u042d\u0442\u0430 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u0431\u0435\u0437 VR: \u043e\u0442\u043a\u043b\u044e\u0447\u0438\u0442\u0435 3D \u0432 Elite \u0438\u043b\u0438 \u0432\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u0418\u0433\u0440\u0430\u0442\u044c \u0431\u0435\u0437 VR."},
    {L"Nothing is changed until you confirm it, and every file replaced is copied into edup_backup\\ first.",
     L"Nichts \u00e4ndert sich, bis du es best\u00e4tigst, und jede ersetzte Datei wird zuerst nach edup_backup\\ kopiert.",
     L"Nada cambia hasta que lo confirmes, y cada archivo sustituido se copia antes en edup_backup\\.",
     L"Rien ne change tant que tu ne confirmes pas, et chaque fichier remplac\u00e9 est d'abord copi\u00e9 dans edup_backup\\.",
     L"Nada muda at\u00e9 voc\u00ea confirmar, e cada arquivo substitu\u00eddo \u00e9 copiado antes para edup_backup\\.",
     L"\u041d\u0438\u0447\u0435\u0433\u043e \u043d\u0435 \u043c\u0435\u043d\u044f\u0435\u0442\u0441\u044f, \u043f\u043e\u043a\u0430 \u0432\u044b \u043d\u0435 \u043f\u043e\u0434\u0442\u0432\u0435\u0440\u0434\u0438\u0442\u0435, \u0438 \u043a\u0430\u0436\u0434\u044b\u0439 \u0437\u0430\u043c\u0435\u043d\u044f\u0435\u043c\u044b\u0439 \u0444\u0430\u0439\u043b \u0441\u043d\u0430\u0447\u0430\u043b\u0430 \u043a\u043e\u043f\u0438\u0440\u0443\u0435\u0442\u0441\u044f \u0432 edup_backup\\."},
    {L"Pick an install above first.", L"W\u00e4hle zuerst oben eine Installation.", L"Elige primero una instalaci\u00f3n arriba.", L"Choisis d'abord une installation ci-dessus.", L"Escolha primeiro uma instala\u00e7\u00e3o acima.", L"\u0421\u043d\u0430\u0447\u0430\u043b\u0430 \u0432\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0443 \u0432\u044b\u0448\u0435."},
    {L"loading", L"wird geladen", L"cargando", L"chargement", L"carregando", L"\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0430"},
    {L"no release downloaded yet", L"noch kein Release geladen", L"a\u00fan no hay una versi\u00f3n descargada", L"aucune version t\u00e9l\u00e9charg\u00e9e", L"nenhuma vers\u00e3o baixada ainda", L"\u0432\u0435\u0440\u0441\u0438\u044f \u0435\u0449\u0451 \u043d\u0435 \u0441\u043a\u0430\u0447\u0430\u043d\u0430"},
    {L"no local package", L"kein lokales Paket", L"sin paquete local", L"aucun paquet local", L"sem pacote local", L"\u043d\u0435\u0442 \u043b\u043e\u043a\u0430\u043b\u044c\u043d\u043e\u0433\u043e \u043f\u0430\u043a\u0435\u0442\u0430"},
    {L"launcher", L"Launcher", L"launcher", L"launcher", L"launcher", L"\u043b\u0430\u0443\u043d\u0447\u0435\u0440"},
    {L"unofficial patch for Elite Dangerous in VR and flat", L"inoffizieller Patch f\u00fcr Elite Dangerous in VR und flach", L"parche no oficial para Elite Dangerous en RV y plano", L"patch non officiel pour Elite Dangerous en RV et \u00e0 plat", L"patch n\u00e3o oficial para Elite Dangerous em RV e plano", L"\u043d\u0435\u043e\u0444\u0438\u0446\u0438\u0430\u043b\u044c\u043d\u044b\u0439 \u043f\u0430\u0442\u0447 \u0434\u043b\u044f Elite Dangerous \u0432 VR \u0438 \u0431\u0435\u0437 VR"},
    {L"Elite is already running.", L"Elite l\u00e4uft bereits.", L"Elite ya se est\u00e1 ejecutando.", L"Elite est d\u00e9j\u00e0 lanc\u00e9.", L"Elite j\u00e1 est\u00e1 em execu\u00e7\u00e3o.", L"Elite \u0443\u0436\u0435 \u0437\u0430\u043f\u0443\u0449\u0435\u043d."},
    {L"Signs in to Epic and Frontier, starts Elite itself, and exits once Elite is running.",
     L"Meldet sich bei Epic und Frontier an, startet Elite selbst und beendet sich, sobald Elite l\u00e4uft.",
     L"Inicia sesi\u00f3n en Epic y Frontier, arranca Elite y se cierra cuando Elite est\u00e1 en marcha.",
     L"Se connecte \u00e0 Epic et Frontier, lance Elite et se ferme une fois Elite lanc\u00e9.",
     L"Entra na Epic e na Frontier, inicia Elite e fecha quando Elite estiver rodando.",
     L"\u0412\u0445\u043e\u0434\u0438\u0442 \u0432 Epic \u0438 Frontier, \u0441\u0430\u043c \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 Elite \u0438 \u0437\u0430\u043a\u0440\u044b\u0432\u0430\u0435\u0442\u0441\u044f, \u043a\u043e\u0433\u0434\u0430 Elite \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442."},
    {L"Signs in itself. If that fails, it starts EDLaunch.",
     L"Meldet sich selbst an. Wenn das scheitert, startet es EDLaunch.",
     L"Inicia sesi\u00f3n por su cuenta. Si falla, arranca EDLaunch.",
     L"Se connecte lui-m\u00eame. En cas d'\u00e9chec, il lance EDLaunch.",
     L"Entra por conta pr\u00f3pria. Se falhar, inicia o EDLaunch.",
     L"\u0412\u0445\u043e\u0434\u0438\u0442 \u0441\u0430\u043c. \u0415\u0441\u043b\u0438 \u043d\u0435 \u0432\u044b\u0439\u0434\u0435\u0442, \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 EDLaunch."},
    {L"Signs in with your Frontier account. If that fails, it starts through Steam.",
     L"Meldet sich mit deinem Frontier-Konto an. Wenn das scheitert, startet es \u00fcber Steam.",
     L"Inicia sesi\u00f3n con tu cuenta de Frontier. Si falla, arranca a trav\u00e9s de Steam.",
     L"Se connecte avec ton compte Frontier. En cas d'\u00e9chec, il lance via Steam.",
     L"Entra com sua conta Frontier. Se falhar, inicia pelo Steam.",
     L"\u0412\u0445\u043e\u0434\u0438\u0442 \u0447\u0435\u0440\u0435\u0437 \u0430\u043a\u043a\u0430\u0443\u043d\u0442 Frontier. \u0415\u0441\u043b\u0438 \u043d\u0435 \u0432\u044b\u0439\u0434\u0435\u0442, \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 \u0447\u0435\u0440\u0435\u0437 Steam."},
    {L"Signs in itself. If that fails, it starts through Steam.",
     L"Meldet sich selbst an. Wenn das scheitert, startet es \u00fcber Steam.",
     L"Inicia sesi\u00f3n por su cuenta. Si falla, arranca a trav\u00e9s de Steam.",
     L"Se connecte lui-m\u00eame. En cas d'\u00e9chec, il lance via Steam.",
     L"Entra por conta pr\u00f3pria. Se falhar, inicia pelo Steam.",
     L"\u0412\u0445\u043e\u0434\u0438\u0442 \u0441\u0430\u043c. \u0415\u0441\u043b\u0438 \u043d\u0435 \u0432\u044b\u0439\u0434\u0435\u0442, \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 \u0447\u0435\u0440\u0435\u0437 Steam."},
    {L"Starts EDLaunch with the mode you pick, then closes it when Elite exits.",
     L"Startet EDLaunch im gew\u00e4hlten Modus und schlie\u00dft es, wenn Elite endet.",
     L"Arranca EDLaunch en el modo que elijas y lo cierra cuando Elite termina.",
     L"Lance EDLaunch dans le mode choisi, puis le ferme quand Elite se termine.",
     L"Inicia o EDLaunch no modo escolhido e o fecha quando Elite termina.",
     L"\u0417\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 EDLaunch \u0432 \u0432\u044b\u0431\u0440\u0430\u043d\u043d\u043e\u043c \u0440\u0435\u0436\u0438\u043c\u0435 \u0438 \u0437\u0430\u043a\u0440\u044b\u0432\u0430\u0435\u0442 \u0435\u0433\u043e, \u043a\u043e\u0433\u0434\u0430 Elite \u0437\u0430\u0432\u0435\u0440\u0448\u0430\u0435\u0442\u0441\u044f."},
    {L"Starts EDLaunch in VR, then closes it when Elite exits.",
     L"Startet EDLaunch in VR und schlie\u00dft es, wenn Elite endet.",
     L"Arranca EDLaunch en RV y lo cierra cuando Elite termina.",
     L"Lance EDLaunch en RV, puis le ferme quand Elite se termine.",
     L"Inicia o EDLaunch em RV e o fecha quando Elite termina.",
     L"\u0417\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 EDLaunch \u0432 VR \u0438 \u0437\u0430\u043a\u0440\u044b\u0432\u0430\u0435\u0442 \u0435\u0433\u043e, \u043a\u043e\u0433\u0434\u0430 Elite \u0437\u0430\u0432\u0435\u0440\u0448\u0430\u0435\u0442\u0441\u044f."},
    {L"Starts EDLaunch in flat, then closes it when Elite exits.",
     L"Startet EDLaunch flach und schlie\u00dft es, wenn Elite endet.",
     L"Arranca EDLaunch en plano y lo cierra cuando Elite termina.",
     L"Lance EDLaunch \u00e0 plat, puis le ferme quand Elite se termine.",
     L"Inicia o EDLaunch em plano e o fecha quando Elite termina.",
     L"\u0417\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 EDLaunch \u0431\u0435\u0437 VR \u0438 \u0437\u0430\u043a\u0440\u044b\u0432\u0430\u0435\u0442 \u0435\u0433\u043e, \u043a\u043e\u0433\u0434\u0430 Elite \u0437\u0430\u0432\u0435\u0440\u0448\u0430\u0435\u0442\u0441\u044f."},
    {L"Update", L"Aktualisieren", L"Actualizar", L"Mettre \u00e0 jour", L"Atualizar", L"\u041e\u0431\u043d\u043e\u0432\u0438\u0442\u044c"},
    {L"Reinstall", L"Neu installieren", L"Reinstalar", L"R\u00e9installer", L"Reinstalar", L"\u041f\u0435\u0440\u0435\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c"},
    {L"Copy path", L"Pfad kopieren", L"Copiar ruta", L"Copier le chemin", L"Copiar caminho", L"\u041a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u0442\u044c \u043f\u0443\u0442\u044c"},
    {L"Show folder", L"Ordner zeigen", L"Mostrar carpeta", L"Afficher le dossier", L"Mostrar pasta", L"\u041f\u043e\u043a\u0430\u0437\u0430\u0442\u044c \u043f\u0430\u043f\u043a\u0443"},
    {L"Search settings", L"Einstellungen suchen", L"Buscar ajustes", L"Chercher un r\u00e9glage", L"Buscar configura\u00e7\u00f5es", L"\u0418\u0441\u043a\u0430\u0442\u044c \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0443"},
    {L"holds d3d11.dll; Install chains both", L"belegt d3d11.dll; Installieren verkettet beide", L"usa d3d11.dll; Instalar encadena ambos", L"occupe d3d11.dll ; Installer encha\u00eene les deux", L"ocupa d3d11.dll; Instalar encadeia ambos", L"\u0437\u0430\u043d\u0438\u043c\u0430\u0435\u0442 d3d11.dll; \u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c \u0441\u0432\u044f\u0436\u0435\u0442 \u043e\u0431\u0430"},
    {L"holds d3d11.dll; Repair chains both", L"belegt d3d11.dll; Reparieren verkettet beide", L"usa d3d11.dll; Reparar encadena ambos", L"occupe d3d11.dll ; R\u00e9parer encha\u00eene les deux", L"ocupa d3d11.dll; Reparar encadeia ambos", L"\u0437\u0430\u043d\u0438\u043c\u0430\u0435\u0442 d3d11.dll; \u0412\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c \u0441\u0432\u044f\u0436\u0435\u0442 \u043e\u0431\u0430"},
    {L"Alongside", L"Daneben", L"Junto a", L"\u00c0 c\u00f4t\u00e9", L"Ao lado", L"\u0420\u044f\u0434\u043e\u043c"},
    {L"another mod", L"ein anderer Mod", L"otro mod", L"un autre mod", L"outro mod", L"\u0434\u0440\u0443\u0433\u043e\u0439 \u043c\u043e\u0434"},
    {L", chained through ", L", verbunden \u00fcber ", L", encadenado a trav\u00e9s de ", L", encha\u00een\u00e9 via ", L", encadeado por ", L", \u0441\u0432\u044f\u0437\u0430\u043d\u043e \u0447\u0435\u0440\u0435\u0437 "},
    {L"Elite Dangerous", L"Elite Dangerous", L"Elite Dangerous", L"Elite Dangerous", L"Elite Dangerous", L"Elite Dangerous"},
    {L"Email", L"E-Mail", L"Correo", L"E-mail", L"E-mail", L"\u041f\u043e\u0447\u0442\u0430"},
    {L"Password", L"Passwort", L"Contrase\u00f1a", L"Mot de passe", L"Senha", L"\u041f\u0430\u0440\u043e\u043b\u044c"},
    {L"Code", L"Code", L"C\u00f3digo", L"Code", L"C\u00f3digo", L"\u041a\u043e\u0434"},
    {L"Remember my password on this PC", L"Passwort auf diesem PC merken", L"Recordar mi contrase\u00f1a en este PC", L"M\u00e9moriser mon mot de passe sur ce PC", L"Lembrar minha senha neste PC", L"\u0417\u0430\u043f\u043e\u043c\u043d\u0438\u0442\u044c \u043f\u0430\u0440\u043e\u043b\u044c \u043d\u0430 \u044d\u0442\u043e\u043c \u041f\u041a"},
    {L"Sign in", L"Anmelden", L"Iniciar sesi\u00f3n", L"Se connecter", L"Entrar", L"\u0412\u043e\u0439\u0442\u0438"},
    {L"Continue", L"Weiter", L"Continuar", L"Continuer", L"Continuar", L"\u0414\u0430\u043b\u0435\u0435"},
    {L"Cancel", L"Abbrechen", L"Cancelar", L"Annuler", L"Cancelar", L"\u041e\u0442\u043c\u0435\u043d\u0430"},
    {L"Enter both the email and the password.", L"Gib E-Mail und Passwort ein.", L"Escribe el correo y la contrase\u00f1a.", L"Saisis l'e-mail et le mot de passe.", L"Digite o e-mail e a senha.", L"\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043f\u043e\u0447\u0442\u0443 \u0438 \u043f\u0430\u0440\u043e\u043b\u044c."},
    {L"Type the code from the email.", L"Gib den Code aus der E-Mail ein.", L"Escribe el c\u00f3digo del correo.", L"Saisis le code re\u00e7u par e-mail.", L"Digite o c\u00f3digo do e-mail.", L"\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043a\u043e\u0434 \u0438\u0437 \u043f\u0438\u0441\u044c\u043c\u0430."},
    {L"Sign in to Frontier", L"Bei Frontier anmelden", L"Iniciar sesi\u00f3n en Frontier", L"Se connecter \u00e0 Frontier", L"Entrar na Frontier", L"\u0412\u043e\u0439\u0442\u0438 \u0432 Frontier"},
    {L"Enter the code from Frontier", L"Code von Frontier eingeben", L"Introduce el c\u00f3digo de Frontier", L"Saisis le code de Frontier", L"Digite o c\u00f3digo da Frontier", L"\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043a\u043e\u0434 \u043e\u0442 Frontier"},
    {L"Frontier emailed a verification code to %s.", L"Frontier hat einen Code an %s geschickt.", L"Frontier envi\u00f3 un c\u00f3digo de verificaci\u00f3n a %s.", L"Frontier a envoy\u00e9 un code de v\u00e9rification \u00e0 %s.", L"A Frontier enviou um c\u00f3digo de verifica\u00e7\u00e3o para %s.", L"Frontier \u043e\u0442\u043f\u0440\u0430\u0432\u0438\u043b\u0430 \u043a\u043e\u0434 \u043f\u043e\u0434\u0442\u0432\u0435\u0440\u0436\u0434\u0435\u043d\u0438\u044f \u043d\u0430 %s."},
    {L"Your password goes only to Frontier's servers. It is stored encrypted for this Windows user, and only if the box is ticked.",
     L"Dein Passwort geht nur an Frontiers Server. Es wird verschl\u00fcsselt f\u00fcr diesen Windows-Benutzer gespeichert, und nur wenn das Feld an ist.",
     L"Tu contrase\u00f1a va solo a los servidores de Frontier. Se guarda cifrada para este usuario de Windows, y solo si la casilla est\u00e1 marcada.",
     L"Ton mot de passe ne va qu'aux serveurs de Frontier. Il est stock\u00e9 chiffr\u00e9 pour cet utilisateur Windows, et seulement si la case est coch\u00e9e.",
     L"Sua senha vai apenas aos servidores da Frontier. Ela \u00e9 guardada criptografada para este usu\u00e1rio do Windows, e s\u00f3 se a caixa estiver marcada.",
     L"\u041f\u0430\u0440\u043e\u043b\u044c \u0443\u0445\u043e\u0434\u0438\u0442 \u0442\u043e\u043b\u044c\u043a\u043e \u043d\u0430 \u0441\u0435\u0440\u0432\u0435\u0440\u044b Frontier. \u041e\u043d \u0445\u0440\u0430\u043d\u0438\u0442\u0441\u044f \u0437\u0430\u0448\u0438\u0444\u0440\u043e\u0432\u0430\u043d\u043d\u044b\u043c \u0434\u043b\u044f \u044d\u0442\u043e\u0433\u043e \u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f Windows \u0438 \u0442\u043e\u043b\u044c\u043a\u043e \u0435\u0441\u043b\u0438 \u0444\u043b\u0430\u0436\u043e\u043a \u0432\u043a\u043b\u044e\u0447\u0451\u043d."},
    {L"Your password goes only to Frontier's servers. Under Wine or Proton the saved password is not strongly protected on this PC.",
     L"Dein Passwort geht nur an Frontiers Server. Unter Wine oder Proton ist das gespeicherte Passwort auf diesem PC nicht stark gesch\u00fctzt.",
     L"Tu contrase\u00f1a va solo a los servidores de Frontier. Bajo Wine o Proton la contrase\u00f1a guardada no est\u00e1 bien protegida en este PC.",
     L"Ton mot de passe ne va qu'aux serveurs de Frontier. Sous Wine ou Proton le mot de passe enregistr\u00e9 n'est pas bien prot\u00e9g\u00e9 sur ce PC.",
     L"Sua senha vai apenas aos servidores da Frontier. Sob Wine ou Proton a senha salva n\u00e3o fica bem protegida neste PC.",
     L"\u041f\u0430\u0440\u043e\u043b\u044c \u0443\u0445\u043e\u0434\u0438\u0442 \u0442\u043e\u043b\u044c\u043a\u043e \u043d\u0430 \u0441\u0435\u0440\u0432\u0435\u0440\u044b Frontier. \u041f\u043e\u0434 Wine \u0438\u043b\u0438 Proton \u0441\u043e\u0445\u0440\u0430\u043d\u0451\u043d\u043d\u044b\u0439 \u043f\u0430\u0440\u043e\u043b\u044c \u043d\u0430 \u044d\u0442\u043e\u043c \u041f\u041a \u0437\u0430\u0449\u0438\u0449\u0451\u043d \u0441\u043b\u0430\u0431\u043e."},
    {L"Sets Elite's 3D mode in Settings.xml, then starts Elite through Steam.",
     L"Setzt Elites 3D-Modus in Settings.xml und startet Elite dann \u00fcber Steam.",
     L"Ajusta el modo 3D de Elite en Settings.xml y luego inicia Elite mediante Steam.",
     L"R\u00e8gle le mode 3D d'Elite dans Settings.xml, puis lance Elite via Steam.",
     L"Define o modo 3D do Elite em Settings.xml e inicia Elite pelo Steam.",
     L"\u0417\u0430\u0434\u0430\u0451\u0442 \u0440\u0435\u0436\u0438\u043c 3D Elite \u0432 Settings.xml, \u0437\u0430\u0442\u0435\u043c \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 Elite \u0447\u0435\u0440\u0435\u0437 Steam."},
    {L"Sets Elite's 3D mode in Settings.xml, then starts Elite through Epic.",
     L"Setzt Elites 3D-Modus in Settings.xml und startet Elite dann \u00fcber Epic.",
     L"Ajusta el modo 3D de Elite en Settings.xml y luego inicia Elite mediante Epic.",
     L"R\u00e8gle le mode 3D d'Elite dans Settings.xml, puis lance Elite via Epic.",
     L"Define o modo 3D do Elite em Settings.xml e inicia Elite pela Epic.",
     L"\u0417\u0430\u0434\u0430\u0451\u0442 \u0440\u0435\u0436\u0438\u043c 3D Elite \u0432 Settings.xml, \u0437\u0430\u0442\u0435\u043c \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 Elite \u0447\u0435\u0440\u0435\u0437 Epic."},
    {L"VR 3D mode", L"VR-3D-Modus", L"Modo 3D RV", L"Mode 3D RV", L"Modo 3D RV", L"\u0420\u0435\u0436\u0438\u043c 3D VR"},
    {L"Not Set", L"Nicht festgelegt", L"Sin definir", L"Non d\u00e9fini", L"N\u00e3o definido", L"\u041d\u0435 \u0437\u0430\u0434\u0430\u043d\u043e"},
    {L"Choose your preferred VR 3D mode before launching in VR.",
     L"W\u00e4hle zuerst deinen bevorzugten VR-3D-Modus, bevor du in VR startest.",
     L"Elige tu modo 3D RV preferido antes de iniciar en RV.",
     L"Choisis ton mode 3D RV pr\u00e9f\u00e9r\u00e9 avant de lancer en RV.",
     L"Escolha seu modo 3D RV preferido antes de iniciar em RV.",
     L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u043f\u0440\u0435\u0434\u043f\u043e\u0447\u0438\u0442\u0430\u0435\u043c\u044b\u0439 \u0440\u0435\u0436\u0438\u043c 3D VR \u043f\u0435\u0440\u0435\u0434 \u0437\u0430\u043f\u0443\u0441\u043a\u043e\u043c \u0432 VR."},
    {L"HMD (Headphones)", L"HMD (Kopfh\u00f6rer)", L"HMD (Auriculares)", L"HMD (Casque)", L"HMD (Fones de ouvido)", L"HMD (\u041d\u0430\u0443\u0448\u043d\u0438\u043a\u0438)"},
    {L"HMD (Speakers)", L"HMD (Lautsprecher)", L"HMD (Altavoces)", L"HMD (Haut-parleurs)", L"HMD (Alto-falantes)", L"HMD (\u0414\u0438\u043d\u0430\u043c\u0438\u043a\u0438)"},
    {L"HMD Cinema (Headphones)", L"HMD-Kino (Kopfh\u00f6rer)", L"Cine HMD (Auriculares)", L"Cin\u00e9ma HMD (Casque)", L"Cinema HMD (Fones)", L"\u041a\u0438\u043d\u043e HMD (\u041d\u0430\u0443\u0448\u043d\u0438\u043a\u0438)"},
    {L"HMD Cinema (Speakers)", L"HMD-Kino (Lautsprecher)", L"Cine HMD (Altavoces)", L"Cin\u00e9ma HMD (Haut-parleurs)", L"Cinema HMD (Alto-falantes)", L"\u041a\u0438\u043d\u043e HMD (\u0414\u0438\u043d\u0430\u043c\u0438\u043a\u0438)"},
    {L"Skip the Epic launcher", L"Epic-Launcher \u00fcberspringen", L"Omitir el launcher de Epic", L"Passer le launcher Epic", L"Pular o launcher da Epic", L"\u041f\u0440\u043e\u043f\u0443\u0441\u0442\u0438\u0442\u044c \u043b\u0430\u0443\u043d\u0447\u0435\u0440 Epic"},
    {L"Ask on Discord", L"Auf Discord fragen", L"Preguntar en Discord", L"Demander sur Discord", L"Perguntar no Discord", L"\u0421\u043f\u0440\u043e\u0441\u0438\u0442\u044c \u0432 Discord"},
    {L"ED-UP installed as openvr_api.dll", L"ED-UP ist als openvr_api.dll installiert", L"ED-UP instalado como openvr_api.dll", L"ED-UP install\u00e9 en tant que openvr_api.dll", L"ED-UP instalado como openvr_api.dll", L"ED-UP \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d \u043a\u0430\u043a openvr_api.dll"},
    {L"no Openvr folder found under this install", L"kein Openvr-Ordner in dieser Installation", L"no hay carpeta Openvr en esta instalaci\u00f3n", L"aucun dossier Openvr dans cette installation", L"nenhuma pasta Openvr nesta instala\u00e7\u00e3o", L"\u043f\u0430\u043f\u043a\u0430 Openvr \u0432 \u044d\u0442\u043e\u0439 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0435 \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u0430"},
    {L"not installed \x00b7 the game's own runtime is in place", L"nicht installiert \x00b7 die Laufzeit des Spiels liegt bereit", L"no instalado \x00b7 el runtime del juego est\u00e1 en su sitio", L"non install\u00e9 \x00b7 le runtime du jeu est en place", L"n\u00e3o instalado \x00b7 o runtime do jogo est\u00e1 no lugar", L"\u043d\u0435 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u043e \x00b7 \u0441\u0440\u0435\u0434\u0430 \u0438\u0433\u0440\u044b \u043d\u0430 \u043c\u0435\u0441\u0442\u0435"},
    {L"Install record", L"Installationsvermerk", L"Registro de instalaci\u00f3n", L"Relev\u00e9 d'installation", L"Registro da instala\u00e7\u00e3o", L"\u0417\u0430\u043f\u0438\u0441\u044c \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438"},
    {L"older than the files here; Update refreshes it", L"\u00e4lter als die Dateien hier; Update erneuert ihn", L"m\u00e1s antiguo que los archivos; Actualizar lo renueva", L"plus ancien que les fichiers ici ; Update le rafra\u00eechit", L"mais antigo que os arquivos; Atualizar o renova", L"\u0441\u0442\u0430\u0440\u0448\u0435 \u0444\u0430\u0439\u043b\u043e\u0432 \u0437\u0434\u0435\u0441\u044c; \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0435 \u0435\u0433\u043e \u043e\u0431\u043d\u043e\u0432\u0438\u0442"},
    {L"The game's runtime", L"Die Laufzeit des Spiels", L"El runtime del juego", L"Le runtime du jeu", L"O runtime do jogo", L"\u0421\u0440\u0435\u0434\u0430 \u0438\u0433\u0440\u044b"},
    {L"the game's own runtime is in place", L"die Laufzeit des Spiels liegt bereit", L"el runtime del juego est\u00e1 en su sitio", L"le runtime du jeu est en place", L"o runtime do jogo est\u00e1 no lugar", L"\u0441\u0440\u0435\u0434\u0430 \u0438\u0433\u0440\u044b \u043d\u0430 \u043c\u0435\u0441\u0442\u0435"},
    {L"safe, renamed openvr_api_orig.dll", L"sicher, umbenannt in openvr_api_orig.dll", L"a salvo, renombrado a openvr_api_orig.dll", L"en s\u00fbret\u00e9, renomm\u00e9 openvr_api_orig.dll", L"a salvo, renomeado para openvr_api_orig.dll", L"\u0432 \u0441\u043e\u0445\u0440\u0430\u043d\u043d\u043e\u0441\u0442\u0438, \u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d \u0432 openvr_api_orig.dll"},
    {L"ED-UP's runtime is in place; flat restores the game's own", L"ED-UPs Laufzeit liegt bereit; flach stellt die des Spiels wieder her", L"el runtime de ED-UP est\u00e1 en su sitio; el modo plano restaura el del juego", L"le runtime d'ED-UP est en place ; le plat restaure celui du jeu", L"o runtime do ED-UP est\u00e1 no lugar; o modo plano restaura o do jogo", L"\u0441\u0440\u0435\u0434\u0430 ED-UP \u043d\u0430 \u043c\u0435\u0441\u0442\u0435; \u0440\u0435\u0436\u0438\u043c \u0431\u0435\u0437 VR \u0432\u043e\u0437\u0432\u0440\u0430\u0449\u0430\u0435\u0442 \u0441\u0440\u0435\u0434\u0443 \u0438\u0433\u0440\u044b"},
    {L"not found", L"nicht gefunden", L"no encontrado", L"introuvable", L"n\u00e3o encontrado", L"\u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u043e"},
    {L"ED-UP's own runtime is in place", L"ED-UPs eigene Laufzeit liegt bereit", L"el runtime propio de ED-UP est\u00e1 en su sitio", L"le runtime propre d'ED-UP est en place", L"o runtime pr\u00f3prio do ED-UP est\u00e1 no lugar", L"\u0441\u043e\u0431\u0441\u0442\u0432\u0435\u043d\u043d\u0430\u044f \u0441\u0440\u0435\u0434\u0430 ED-UP \u043d\u0430 \u043c\u0435\u0441\u0442\u0435"},
    {L"the game's own runtime; the install replaces it", L"die Laufzeit des Spiels; die Installation ersetzt sie", L"el runtime del juego; la instalaci\u00f3n lo sustituye", L"le runtime du jeu ; l'installation le remplace", L"o runtime do jogo; a instala\u00e7\u00e3o o substitui", L"\u0441\u0440\u0435\u0434\u0430 \u0438\u0433\u0440\u044b; \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u0435\u0451 \u0437\u0430\u043c\u0435\u043d\u0438\u0442"},
    {L"Your settings", L"Deine Einstellungen", L"Tus ajustes", L"Tes r\u00e9glages", L"Suas configura\u00e7\u00f5es", L"\u0412\u0430\u0448\u0438 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438"},
    {L"edup.ini kept", L"edup.ini behalten", L"edup.ini conservado", L"edup.ini conserv\u00e9", L"edup.ini mantido", L"edup.ini \u0441\u043e\u0445\u0440\u0430\u043d\u0451\u043d"},
    {L"edup.ini not yet", L"edup.ini noch nicht", L"edup.ini a\u00fan no", L"edup.ini pas encore", L"edup.ini ainda n\u00e3o", L"edup.ini \u0435\u0449\u0451 \u043d\u0435\u0442"},
    {L"edup-flat.ini kept", L"edup-flat.ini behalten", L"edup-flat.ini conservado", L"edup-flat.ini conserv\u00e9", L"edup-flat.ini mantido", L"edup-flat.ini \u0441\u043e\u0445\u0440\u0430\u043d\u0451\u043d"},
    {L"edup-flat.ini not yet", L"edup-flat.ini noch nicht", L"edup-flat.ini a\u00fan no", L"edup-flat.ini pas encore", L"edup-flat.ini ainda n\u00e3o", L"edup-flat.ini \u0435\u0449\u0451 \u043d\u0435\u0442"},
    {L"edup-flat.ini not yet (edup.ini is copied into it)", L"edup-flat.ini noch nicht (edup.ini wird hineinkopiert)", L"edup-flat.ini a\u00fan no (edup.ini se copia en \u00e9l)", L"edup-flat.ini pas encore (edup.ini y est copi\u00e9)", L"edup-flat.ini ainda n\u00e3o (edup.ini \u00e9 copiado para ele)", L"edup-flat.ini \u0435\u0449\u0451 \u043d\u0435\u0442 (edup.ini \u043a\u043e\u043f\u0438\u0440\u0443\u0435\u0442\u0441\u044f \u0432 \u043d\u0435\u0433\u043e)"},
    {L"running \x2014 close it before installing anything", L"l\u00e4uft \x2014 schlie\u00dfe es, bevor du etwas installierst", L"en ejecuci\u00f3n \x2014 ci\u00e9rralo antes de instalar", L"en cours \x2014 ferme-le avant d'installer", L"em execu\u00e7\u00e3o \x2014 feche antes de instalar", L"\u0437\u0430\u043f\u0443\u0449\u0435\u043d\u043e \x2014 \u0437\u0430\u043a\u0440\u043e\u0439\u0442\u0435 \u043f\u0435\u0440\u0435\u0434 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u043e\u0439"},
    {L"running from a different folder \x2014 not this install", L"l\u00e4uft aus einem anderen Ordner \x2014 nicht diese Installation", L"en ejecuci\u00f3n desde otra carpeta \x2014 no esta instalaci\u00f3n", L"en cours depuis un autre dossier \x2014 pas cette installation", L"em execu\u00e7\u00e3o de outra pasta \x2014 n\u00e3o esta instala\u00e7\u00e3o", L"\u0437\u0430\u043f\u0443\u0449\u0435\u043d\u043e \u0438\u0437 \u0434\u0440\u0443\u0433\u043e\u0439 \u043f\u0430\u043f\u043a\u0438 \x2014 \u043d\u0435 \u044d\u0442\u0430 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430"},
    {L"could not check whether it is running \x2014 nothing will be installed", L"konnte nicht pr\u00fcfen, ob es l\u00e4uft \x2014 es wird nichts installiert", L"no se pudo comprobar si est\u00e1 en ejecuci\u00f3n \x2014 no se instalar\u00e1 nada", L"impossible de v\u00e9rifier s'il tourne \x2014 rien ne sera install\u00e9", L"n\u00e3o foi poss\u00edvel verificar se est\u00e1 em execu\u00e7\u00e3o \x2014 nada ser\u00e1 instalado", L"\u043d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043f\u0440\u043e\u0432\u0435\u0440\u0438\u0442\u044c, \u0437\u0430\u043f\u0443\u0449\u0435\u043d\u043e \u043b\u0438 \x2014 \u043d\u0438\u0447\u0435\u0433\u043e \u043d\u0435 \u0431\u0443\u0434\u0435\u0442 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u043e"},
    {L"ED-UP is installed in this folder, but this launcher has no local release package yet.\r\n\r\nPress Download when Updates shows a release to enable repair and updates, or start the launcher with --payload and the folder of an unpacked ed-up zip.",
     L"ED-UP ist in diesem Ordner installiert, aber dieser Launcher hat noch kein lokales Paket.\r\n\r\nKlicke bei Aktualisierungen auf Herunterladen, um Reparatur und Updates zu aktivieren, oder starte den Launcher mit --payload und dem Ordner eines entpackten ED-UP-Zips.",
     L"ED-UP est\u00e1 instalado en esta carpeta, pero este launcher a\u00fan no tiene un paquete local.\r\n\r\nPulsa Descargar en Actualizaciones para activar la reparaci\u00f3n y las actualizaciones, o inicia el launcher con --payload y la carpeta de un zip de ED-UP descomprimido.",
     L"ED-UP est install\u00e9 dans ce dossier, mais ce launcher n'a pas encore de paquet local.\r\n\r\nClique sur T\u00e9l\u00e9charger dans Mises \u00e0 jour pour activer la r\u00e9paration et les mises \u00e0 jour, ou lance le launcher avec --payload et le dossier d'un zip ED-UP d\u00e9compress\u00e9.",
     L"O ED-UP est\u00e1 instalado nesta pasta, mas este launcher ainda n\u00e3o tem um pacote local.\r\n\r\nClique em Baixar em Atualiza\u00e7\u00f5es para ativar reparo e atualiza\u00e7\u00f5es, ou inicie o launcher com --payload e a pasta de um zip do ED-UP descompactado.",
     L"ED-UP \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d \u0432 \u044d\u0442\u043e\u0439 \u043f\u0430\u043f\u043a\u0435, \u043d\u043e \u0443 \u043b\u0430\u0443\u043d\u0447\u0435\u0440\u0430 \u043f\u043e\u043a\u0430 \u043d\u0435\u0442 \u043b\u043e\u043a\u0430\u043b\u044c\u043d\u043e\u0433\u043e \u043f\u0430\u043a\u0435\u0442\u0430.\r\n\r\n\u041d\u0430\u0436\u043c\u0438\u0442\u0435 \u0421\u043a\u0430\u0447\u0430\u0442\u044c \u0432 \u0440\u0430\u0437\u0434\u0435\u043b\u0435 \u041e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f \u0434\u043b\u044f \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f \u0438 \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0439 \u0438\u043b\u0438 \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u0435 \u043b\u0430\u0443\u043d\u0447\u0435\u0440 \u0441 --payload \u0438 \u043f\u0430\u043f\u043a\u043e\u0439 \u0440\u0430\u0441\u043f\u0430\u043a\u043e\u0432\u0430\u043d\u043d\u043e\u0433\u043e zip ED-UP."},
    {L"There is nothing to install from yet.\r\n\r\nPress Download when Updates shows a release, or start the launcher with --payload and the folder of an unpacked ed-up zip.",
     L"Es ist noch kein Paket zum Installieren vorhanden.\r\n\r\nKlicke bei Aktualisierungen auf Herunterladen, sobald ein Release angezeigt wird, oder starte den Launcher mit --payload und dem Ordner eines entpackten ED-UP-Zips.",
     L"A\u00fan no hay nada que instalar.\r\n\r\nPulsa Descargar cuando Actualizaciones muestre una versi\u00f3n, o inicia el launcher con --payload y la carpeta de un zip de ED-UP descomprimido.",
     L"Il n'y a encore rien \u00e0 installer.\r\n\r\nClique sur T\u00e9l\u00e9charger quand Mises \u00e0 jour affiche une version, ou lance le launcher avec --payload et le dossier d'un zip ED-UP d\u00e9compress\u00e9.",
     L"Ainda n\u00e3o h\u00e1 nada para instalar.\r\n\r\nClique em Baixar quando Atualiza\u00e7\u00f5es mostrar uma vers\u00e3o, ou inicie o launcher com --payload e a pasta de um zip do ED-UP descompactado.",
     L"\u041f\u043e\u043a\u0430 \u043d\u0435\u0447\u0435\u0433\u043e \u0443\u0441\u0442\u0430\u043d\u0430\u0432\u043b\u0438\u0432\u0430\u0442\u044c.\r\n\r\n\u041d\u0430\u0436\u043c\u0438\u0442\u0435 \u0421\u043a\u0430\u0447\u0430\u0442\u044c, \u043a\u043e\u0433\u0434\u0430 \u0432 \u0440\u0430\u0437\u0434\u0435\u043b\u0435 \u041e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f \u043f\u043e\u044f\u0432\u0438\u0442\u0441\u044f \u0432\u0435\u0440\u0441\u0438\u044f, \u0438\u043b\u0438 \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u0435 \u043b\u0430\u0443\u043d\u0447\u0435\u0440 \u0441 --payload \u0438 \u043f\u0430\u043f\u043a\u043e\u0439 \u0440\u0430\u0441\u043f\u0430\u043a\u043e\u0432\u0430\u043d\u043d\u043e\u0433\u043e zip ED-UP."},
    {L"Checking for updates...", L"Suche nach Updates...", L"Buscando actualizaciones...", L"Recherche de mises \u00e0 jour...", L"Procurando atualiza\u00e7\u00f5es...", L"\u041f\u043e\u0438\u0441\u043a \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0439..."},
    {L"ED-UP %s is available.", L"ED-UP %s ist verf\u00fcgbar.", L"ED-UP %s est\u00e1 disponible.", L"ED-UP %s est disponible.", L"ED-UP %s est\u00e1 dispon\u00edvel.", L"ED-UP %s \u0434\u043e\u0441\u0442\u0443\u043f\u0435\u043d."},
    {L"ED-UP %s package available for repair and updates.", L"ED-UP %s Paket f\u00fcr Reparatur und Updates verf\u00fcgbar.", L"Paquete de ED-UP %s disponible para reparar y actualizar.", L"Paquet ED-UP %s disponible pour r\u00e9parer et mettre \u00e0 jour.", L"Pacote ED-UP %s dispon\u00edvel para reparo e atualiza\u00e7\u00f5es.", L"\u041f\u0430\u043a\u0435\u0442 ED-UP %s \u0434\u043e\u0441\u0442\u0443\u043f\u0435\u043d \u0434\u043b\u044f \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f \u0438 \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0439."},
    {L"ED-UP %s is ready to install.", L"ED-UP %s ist bereit zur Installation.", L"ED-UP %s est\u00e1 listo para instalar.", L"ED-UP %s est pr\u00eat \u00e0 \u00eatre install\u00e9.", L"ED-UP %s est\u00e1 pronto para instalar.", L"ED-UP %s \u0433\u043e\u0442\u043e\u0432 \u043a \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0435."},
    {L"Updating the launcher...", L"Launcher wird aktualisiert...", L"Actualizando el launcher...", L"Mise \u00e0 jour du launcher...", L"Atualizando o launcher...", L"\u041e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0435 \u043b\u0430\u0443\u043d\u0447\u0435\u0440\u0430..."},
    {L"Starting the update...", L"Update wird gestartet...", L"Iniciando la actualizaci\u00f3n...", L"D\u00e9marrage de la mise \u00e0 jour...", L"Iniciando a atualiza\u00e7\u00e3o...", L"\u0417\u0430\u043f\u0443\u0441\u043a \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f..."},
    {L"The launcher was updated. Close this window and start ED-UP again to carry on.", L"Der Launcher wurde aktualisiert. Schlie\u00dfe dieses Fenster und starte ED-UP erneut.", L"El launcher se actualiz\u00f3. Cierra esta ventana y vuelve a iniciar ED-UP.", L"Le launcher a \u00e9t\u00e9 mis \u00e0 jour. Ferme cette fen\u00eatre et relance ED-UP.", L"O launcher foi atualizado. Feche esta janela e inicie o ED-UP de novo.", L"\u041b\u0430\u0443\u043d\u0447\u0435\u0440 \u043e\u0431\u043d\u043e\u0432\u043b\u0451\u043d. \u0417\u0430\u043a\u0440\u043e\u0439\u0442\u0435 \u044d\u0442\u043e \u043e\u043a\u043d\u043e \u0438 \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u0435 ED-UP \u0441\u043d\u043e\u0432\u0430."},
    {L"Written into the ", L"Geschrieben in die ", L"Escrito en el ", L"\u00c9crit dans le ", L"Gravado no ", L"\u0417\u0430\u043f\u0438\u0441\u0430\u043d\u043e \u0432 "},
    {L" of the install above, and live within a second. The few that need a game restart are marked.",
     L" der Installation oben und innerhalb einer Sekunde wirksam. Die wenigen, die einen Neustart brauchen, sind markiert.",
     L" de la instalaci\u00f3n de arriba, y en vigor en un segundo. Los pocos que necesitan reiniciar el juego est\u00e1n marcados.",
     L" de l'installation ci-dessus, et pris en compte dans la seconde. Les quelques-uns qui demandent un red\u00e9marrage sont marqu\u00e9s.",
     L" da instala\u00e7\u00e3o acima, e em vigor em um segundo. Os poucos que precisam reiniciar o jogo est\u00e3o marcados.",
     L" \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438 \u0432\u044b\u0448\u0435 \u0438 \u0432\u0441\u0442\u0443\u043f\u0430\u0435\u0442 \u0432 \u0441\u0438\u043b\u0443 \u0437\u0430 \u0441\u0435\u043a\u0443\u043d\u0434\u0443. \u041d\u0435\u043c\u043d\u043e\u0433\u0438\u0435, \u043a\u043e\u0442\u043e\u0440\u044b\u043c \u043d\u0443\u0436\u0435\u043d \u043f\u0435\u0440\u0435\u0437\u0430\u043f\u0443\u0441\u043a, \u043e\u0442\u043c\u0435\u0447\u0435\u043d\u044b."},
    {L"EDHM compatibility",
     L"EDHM-Kompatibilit\u00e4t",
     L"Compatibilidad con EDHM",
     L"Compatibilit\u00e9 EDHM",
     L"Compatibilidade com EDHM",
     L"\u0421\u043e\u0432\u043c\u0435\u0441\u0442\u0438\u043c\u043e\u0441\u0442\u044c \u0441 EDHM"},
    {L"(Experimental) Use tailored 2DMigoto dll for EDHM compatibility improvements.",
     L"(Experimentell) Angepasste 2DMigoto-DLL f\u00fcr bessere EDHM-Kompatibilit\u00e4t verwenden.",
     L"(Experimental) Usar la DLL 2DMigoto adaptada para mejorar la compatibilidad con EDHM.",
     L"(Exp\u00e9rimental) Utiliser la DLL 2DMigoto adapt\u00e9e pour am\u00e9liorer la compatibilit\u00e9 EDHM.",
     L"(Experimental) Usar a DLL 2DMigoto adaptada para melhorias de compatibilidade com EDHM.",
     L"(\u042d\u043a\u0441\u043f\u0435\u0440\u0438\u043c\u0435\u043d\u0442\u0430\u043b\u044c\u043d\u043e) \u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c \u0430\u0434\u0430\u043f\u0442\u0438\u0440\u043e\u0432\u0430\u043d\u043d\u0443\u044e DLL 2DMigoto \u0434\u043b\u044f \u0443\u043b\u0443\u0447\u0448\u0435\u043d\u0438\u044f \u0441\u043e\u0432\u043c\u0435\u0441\u0442\u0438\u043c\u043e\u0441\u0442\u0438 \u0441 EDHM."},
    {L"Backs up your standard d3d11_edhm.dll and d3dx.ini settings, installs the signed 2DMigoto build, and applies ED-UP compatibility settings. Turning off restores them.",
     L"Sichert deine regul\u00e4re d3d11_edhm.dll und d3dx.ini-Einstellungen, installiert den signierten 2DMigoto-Build und wendet ED-UP-Kompatibilit\u00e4tseinstellungen an. Ausschalten stellt sie wieder her.",
     L"Hace una copia de seguridad de tu d3d11_edhm.dll est\u00e1ndar y ajustes de d3dx.ini, instala la compilaci\u00f3n firmada 2DMigoto y aplica los ajustes de compatibilidad con ED-UP. Desactivarlo los restaura.",
     L"Sauvegarde votre d3d11_edhm.dll standard et les r\u00e9glages de d3dx.ini, installe la version sign\u00e9e 2DMigoto et applique les r\u00e9glages de compatibilit\u00e9 ED-UP. D\u00e9sactiver les restaure.",
     L"Faz backup da sua d3d11_edhm.dll padr\u00e3o e das op\u00e7\u00f5es de d3dx.ini, instala a compila\u00e7\u00e3o assinada 2DMigoto e aplica os ajustes de compatibilidade com ED-UP. Desativar restaura tudo.",
     L"\u0421\u043e\u0445\u0440\u0430\u043d\u044f\u0435\u0442 \u0440\u0435\u0437\u0435\u0440\u0432\u043d\u0443\u044e \u043a\u043e\u043f\u0438\u044e \u0432\u0430\u0448\u0435\u0439 \u0441\u0442\u0430\u043d\u0434\u0430\u0440\u0442\u043d\u043e\u0439 d3d11_edhm.dll \u0438 \u043f\u0430\u0440\u0430\u043c\u0435\u0442\u0440\u043e\u0432 d3dx.ini, \u0443\u0441\u0442\u0430\u043d\u0430\u0432\u043b\u0438\u0432\u0430\u0435\u0442 \u043f\u043e\u0434\u043f\u0438\u0441\u0430\u043d\u043d\u0443\u044e \u0441\u0431\u043e\u0440\u043a\u0443 2DMigoto \u0438 \u043f\u0440\u0438\u043c\u0435\u043d\u044f\u0435\u0442 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u0441\u043e\u0432\u043c\u0435\u0441\u0442\u0438\u043c\u043e\u0441\u0442\u0438 ED-UP. \u041e\u0442\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442 \u0438\u0445."},
    {L"Hide Refresh/Reload messages from EDHM",
     L"Aktualisierungs-/Neulade-Meldungen von EDHM ausblenden",
     L"Ocultar mensajes de actualizaci\u00f3n/recarga de EDHM",
     L"Masquer les messages d'actualisation/rechargement d'EDHM",
     L"Ocultar mensagens de atualiza\u00e7\u00e3o/recarga do EDHM",
     L"\u0421\u043a\u0440\u044b\u0442\u044c \u0441\u043e\u043e\u0431\u0449\u0435\u043d\u0438\u044f \u043e\u0431\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f/\u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0438 EDHM"},
    {L"Sets [Logging] show_reload_message = 0 in d3dx.ini while 2DMigoto is active; unchecking or turning 2DMigoto off restores your original setting.",
     L"Setzt [Logging] show_reload_message = 0 in d3dx.ini, solange 2DMigoto aktiv ist; Deaktivieren stellt den urspr\u00fcnglichen Wert wieder her.",
     L"Establece [Logging] show_reload_message = 0 en d3dx.ini mientras 2DMigoto est\u00e1 activo; desmarcarlo o desactivar 2DMigoto restaura tu ajuste original.",
     L"D\u00e9finit [Logging] show_reload_message = 0 dans d3dx.ini tant que 2DMigoto est actif ; d\u00e9cocher ou d\u00e9sactiver 2DMigoto restaure le r\u00e9glage d'origine.",
     L"Define [Logging] show_reload_message = 0 em d3dx.ini enquanto 2DMigoto estiver ativo; desmarcar ou desativar o 2DMigoto restaura o valor original.",
     L"\u0417\u0430\u0434\u0430\u0451\u0442 [Logging] show_reload_message = 0 \u0432 d3dx.ini, \u043f\u043e\u043a\u0430 \u0430\u043a\u0442\u0438\u0432\u0435\u043d 2DMigoto; \u043e\u0442\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442 \u0438\u0441\u0445\u043e\u0434\u043d\u043e\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435."},
    {L"Hide Warnings from EDHM",
     L"Warnungen von EDHM ausblenden",
     L"Ocultar advertencias de EDHM",
     L"Masquer les avertissements d'EDHM",
     L"Ocultar avisos do EDHM",
     L"\u0421\u043a\u0440\u044b\u0442\u044c \u043f\u0440\u0435\u0434\u0443\u043f\u0440\u0435\u0436\u0434\u0435\u043d\u0438\u044f EDHM"},
    {L"Sets [Logging] show_warnings = 0 in d3dx.ini while 2DMigoto is active; unchecking or turning 2DMigoto off restores your original setting.",
     L"Setzt [Logging] show_warnings = 0 in d3dx.ini, solange 2DMigoto aktiv ist; Deaktivieren stellt den urspr\u00fcnglichen Wert wieder her.",
     L"Establece [Logging] show_warnings = 0 en d3dx.ini mientras 2DMigoto est\u00e1 activo; desmarcarlo o desactivar 2DMigoto restaura tu ajuste original.",
     L"D\u00e9finit [Logging] show_warnings = 0 dans d3dx.ini tant que 2DMigoto est actif ; d\u00e9cocher ou d\u00e9sactiver 2DMigoto restaure le r\u00e9glage d'origine.",
     L"Define [Logging] show_warnings = 0 em d3dx.ini enquanto 2DMigoto estiver ativo; desmarcar ou desativar o 2DMigoto restaura o valor original.",
     L"\u0417\u0430\u0434\u0430\u0451\u0442 [Logging] show_warnings = 0 \u0432 d3dx.ini, \u043f\u043e\u043a\u0430 \u0430\u043a\u0442\u0438\u0432\u0435\u043d 2DMigoto; \u043e\u0442\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442 \u0438\u0441\u0445\u043e\u0434\u043d\u043e\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435."},
    {L"Requires 2DMigoto",
     L"Erfordert 2DMigoto",
     L"Requiere 2DMigoto",
     L"N\u00e9cessite 2DMigoto",
     L"Requer 2DMigoto",
     L"\u0422\u0440\u0435\u0431\u0443\u0435\u0442\u0441\u044f 2DMigoto"},
    {L"d3dx.ini not found",
     L"d3dx.ini nicht gefunden",
     L"d3dx.ini no encontrado",
     L"d3dx.ini introuvable",
     L"d3dx.ini n\u00e3o encontrado",
     L"d3dx.ini \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d"},
    {L"d3dx.ini unreadable",
     L"d3dx.ini nicht lesbar",
     L"d3dx.ini ilegible",
     L"d3dx.ini illisible",
     L"d3dx.ini ileg\u00edvel",
     L"d3dx.ini \u043d\u0435 \u0447\u0438\u0442\u0430\u0435\u0442\u0441\u044f"},
    {L"d3dx.ini read-only",
     L"d3dx.ini schreibgesch\u00fctzt",
     L"d3dx.ini de solo lectura",
     L"d3dx.ini en lecture seule",
     L"d3dx.ini somente leitura",
     L"d3dx.ini \u0442\u043e\u043b\u044c\u043a\u043e \u0434\u043b\u044f \u0447\u0442\u0435\u043d\u0438\u044f"},
    {L"d3dx.ini unsupported encoding",
     L"d3dx.ini nicht unterst\u00fctzte Kodierung",
     L"Codificaci\u00f3n de d3dx.ini no compatible",
     L"Encodage de d3dx.ini non pris en charge",
     L"Codifica\u00e7\u00e3o de d3dx.ini n\u00e3o suportada",
     L"\u041d\u0435\u043f\u043e\u0434\u0434\u0435\u0440\u0436\u0438\u0432\u0430\u0435\u043c\u0430\u044f \u043a\u043e\u0434\u0438\u0440\u043e\u0432\u043a\u0430 d3dx.ini"},
    {L"Close Elite first",
     L"Elite zuerst schlie\u00dfen",
     L"Cierra Elite primero",
     L"Fermez d'abord Elite",
     L"Feche o Elite primeiro",
     L"\u0421\u043d\u0430\u0447\u0430\u043b\u0430 \u0437\u0430\u043a\u0440\u043e\u0439\u0442\u0435 Elite"},
    {L"Hidden",
     L"Ausgeblendet",
     L"Oculto",
     L"Masqu\u00e9",
     L"Oculto",
     L"\u0421\u043a\u0440\u044b\u0442\u043e"},
    {L"Shown",
     L"Angezeigt",
     L"Mostrado",
     L"Affich\u00e9",
     L"Exibido",
     L"\u041f\u043e\u043a\u0430\u0437\u0430\u043d\u043e"},
    {L"Something has interfered with the download, please check your antivirus and try again.",
     L"Etwas hat den Download beeintr\u00e4chtigt; bitte pr\u00fcfe dein Antivirenprogramm und versuche es erneut.",
     L"Algo ha interferido con la descarga; comprueba tu antivirus e int\u00e9ntalo de nuevo.",
     L"Quelque chose a interf\u00e9r\u00e9 avec le t\u00e9l\u00e9chargement ; v\u00e9rifie ton antivirus et r\u00e9essaie.",
     L"Algo interferiu no download; verifique seu antiv\u00edrus e tente novamente.",
     L"\u0427\u0442\u043e-\u0442\u043e \u043f\u043e\u043c\u0435\u0448\u0430\u043b\u043e \u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0435; \u043f\u0440\u043e\u0432\u0435\u0440\u044c\u0442\u0435 \u0430\u043d\u0442\u0438\u0432\u0438\u0440\u0443\u0441 \u0438 \u043f\u043e\u043f\u0440\u043e\u0431\u0443\u0439\u0442\u0435 \u0441\u043d\u043e\u0432\u0430."},
    {L"No install selected", L"Keine Installation gew\u00e4hlt", L"Ninguna instalaci\u00f3n seleccionada", L"Aucune installation s\u00e9lectionn\u00e9e", L"Nenhuma instala\u00e7\u00e3o selecionada", L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u043d\u0435 \u0432\u044b\u0431\u0440\u0430\u043d\u0430"},
    {L"Applying...", L"Wird angewendet...", L"Aplicando...", L"Application...", L"Aplicando...", L"\u041f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u0438\u0435..."},
    {L"2DMigoto active", L"2DMigoto aktiv", L"2DMigoto activo", L"2DMigoto actif", L"2DMigoto ativo", L"2DMigoto \u0430\u043a\u0442\u0438\u0432\u0435\u043d"},
    {L"2DMigoto still on disk", L"2DMigoto noch auf der Festplatte", L"2DMigoto a\u00fan en disco", L"2DMigoto encore sur disque", L"2DMigoto ainda no disco", L"2DMigoto \u0432\u0441\u0451 \u0435\u0449\u0451 \u043d\u0430 \u0434\u0438\u0441\u043a\u0435"},
    {L"Restore standard", L"Standard wiederherstellen", L"Restaurar est\u00e1ndar", L"Restaurer le standard", L"Restaurar padr\u00e3o", L"\u0412\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c \u0441\u0442\u0430\u043d\u0434\u0430\u0440\u0442"},
    {L"Download a release first", L"Zuerst ein Release herunterladen", L"Descarga primero una versi\u00f3n", L"T\u00e9l\u00e9charger d'abord une version", L"Baixe primeiro uma vers\u00e3o", L"\u0421\u043d\u0430\u0447\u0430\u043b\u0430 \u0441\u043a\u0430\u0447\u0430\u0439\u0442\u0435 \u0440\u0435\u043b\u0438\u0437"},
    {L"Download required", L"Download erforderlich", L"Descarga necesaria", L"T\u00e9l\u00e9chargement requis", L"Download necess\u00e1rio", L"\u0422\u0440\u0435\u0431\u0443\u0435\u0442\u0441\u044f \u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0430"},
    {L"Download now", L"Jetzt herunterladen", L"Descargar ahora", L"T\u00e9l\u00e9charger maintenant", L"Baixar agora", L"\u0421\u043a\u0430\u0447\u0430\u0442\u044c \u0441\u0435\u0439\u0447\u0430\u0441"},
    {L"Checking...", L"Wird gepr\u00fcft...", L"Comprobando...", L"V\u00e9rification...", L"Verificando...", L"\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u0430..."},
    {L"Not in this release", L"Nicht in diesem Release", L"No en esta versi\u00f3n", L"Absent de cette version", L"N\u00e3o est\u00e1 nesta vers\u00e3o", L"\u041d\u0435\u0442 \u0432 \u044d\u0442\u043e\u043c \u0440\u0435\u043b\u0438\u0437\u0435"},
    {L"EDHM not detected", L"EDHM nicht erkannt", L"EDHM no detectado", L"EDHM non d\u00e9tect\u00e9", L"EDHM n\u00e3o detectado", L"EDHM \u043d\u0435 \u043e\u0431\u043d\u0430\u0440\u0443\u0436\u0435\u043d"},
    {L"EDHM updated externally", L"EDHM extern aktualisiert", L"EDHM actualizado externamente", L"EDHM mis \u00e0 jour en externe", L"EDHM atualizado externamente", L"EDHM \u043e\u0431\u043d\u043e\u0432\u043b\u0451\u043d \u0438\u0437\u0432\u043d\u0435"},
    {L"Pending install", L"Installation ausstehend", L"Instalaci\u00f3n pendiente", L"Installation en attente", L"Instala\u00e7\u00e3o pendente", L"\u041e\u0436\u0438\u0434\u0430\u0435\u0442 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438"},
    {L"Apply now", L"Jetzt anwenden", L"Aplicar ahora", L"Appliquer maintenant", L"Aplicar agora", L"\u041f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u044c \u0441\u0435\u0439\u0447\u0430\u0441"},
    {L"Standard EDHM", L"Standard-EDHM", L"EDHM est\u00e1ndar", L"EDHM standard", L"EDHM padr\u00e3o", L"\u0421\u0442\u0430\u043d\u0434\u0430\u0440\u0442\u043d\u044b\u0439 EDHM"},
    {L"Off", L"Aus", L"Desactivado", L"D\u00e9sactiv\u00e9", L"Desativado", L"\u0412\u044b\u043a\u043b."},
};

std::string g_code = "en";

const wchar_t* column(const Row& row) {
    if (g_code == "de") return row.de;
    if (g_code == "es") return row.es;
    if (g_code == "fr") return row.fr;
    if (g_code == "pt-BR") return row.pt;
    if (g_code == "ru") return row.ru;
    return row.en;
}

}  // namespace

void l10nSet(const std::string& code) {
    g_code = "en";
    if (code == "de" || code == "es" || code == "fr" || code == "pt-BR" || code == "ru") g_code = code;
}

const std::string& l10nCode() { return g_code; }

const wchar_t* tr(const wchar_t* english) {
    if (!english || g_code == "en") return english;
    for (const Row& row : kRows) {
        if (wcscmp(row.en, english) == 0) return column(row);
    }
    return english;
}

namespace {

std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::wstring fillVersion(const wchar_t* pattern, const std::wstring& version) {
    std::wstring text = pattern ? pattern : L"";
    const size_t at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, version);
    return text;
}

bool takeMarked(const std::string& line, const char* head, const char* tail, std::string* middle) {
    const size_t h = std::strlen(head);
    const size_t t = std::strlen(tail);
    if (line.size() < h + t || line.compare(0, h, head) != 0 || line.compare(line.size() - t, t, tail) != 0) return false;
    *middle = line.substr(h, line.size() - h - t);
    return true;
}

}  // namespace

std::wstring trLine(const std::string& utf8) {
    std::string version;
    if (takeMarked(utf8, "ED-UP ", " is available.", &version))
        return fillVersion(tr(L"ED-UP %s is available."), widenUtf8(version));
    if (takeMarked(utf8, "ED-UP ", " package available for repair and updates.", &version))
        return fillVersion(tr(L"ED-UP %s package available for repair and updates."), widenUtf8(version));
    if (takeMarked(utf8, "ED-UP ", " is ready to install.", &version))
        return fillVersion(tr(L"ED-UP %s is ready to install."), widenUtf8(version));
    const std::wstring wide = widenUtf8(utf8);
    const wchar_t* hit = tr(wide.c_str());
    return hit == wide.c_str() ? wide : std::wstring(hit);
}

}  // namespace edvr::installer
