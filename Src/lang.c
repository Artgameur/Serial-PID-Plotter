// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Language of the UI: translations of the English texts of the code (UTF-8).

   A text missing from the table stays in English. A translation keeps the printf conversions of
   its English text (%s, %lu...), in the same order. Dropdown options keep their order.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "lang.h"

#include <string.h>

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  const char * en;
  const char * fr;
} Translation_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static Lang_t language = LANG_EN;

static const Translation_t translations[] =
{
  // Tabs, top bar, settings panel
  {"Serial", "Série"},
  {"Plot", "Graphique"},
  {"Settings", "Paramètres"},
  {"File", "Fichier"},
  {"Whole setup\nPlot layout", "Configuration complète\nDisposition des graphiques"},
  {"Whole setup: as a profile (port, variables, plots, commands, tests). Plot layout: the plots, their windows and which variables they show.", "Configuration complète : comme un profil (port, variables, graphiques, commandes, essais). Disposition des graphiques : les graphiques, leurs fenêtres et les variables qu'ils affichent."},
  {"Export", "Exporter"},
  {"Saved: %s", "Enregistré : %s"},
  {"Import", "Importer"},
  {"Save it to a file", "L'enregistrer dans un fichier"},
  {"Open a setup or plot layout file (restarts the app)", "Ouvrir un fichier de configuration ou de disposition (redémarre l'application)"},
  {"Export the settings", "Exporter les paramètres"},
  {"Import settings", "Importer des paramètres"},
  {"%s is not a setup or plot layout file exported by the app.", "%s n'est pas un fichier de configuration ou de disposition exporté par l'application."},
  {"Imported: close and start the app again to use it.", "Importé : fermez et relancez l'application pour l'utiliser."},
  {"Help", "Aide"},
  {"About", "À propos"},
  {"Author: artgameur", "Auteur : artgameur"},
  {"How to use the app, author", "Utilisation de l'application, auteur"},
  {"Choose the port and the baud rate, then Connect. What the device sends shows in the terminal; type a line at the bottom to send it. Several devices at once: choose the number of Connections, each with its own port.", "Choisissez le port et la vitesse (baud), puis Connecter. Ce que l'appareil envoie s'affiche dans le terminal; tapez une ligne en bas pour l'envoyer. Plusieurs appareils à la fois : choisissez le nombre de Connexions, chacune avec son port."},
  {"Each line received is split into numbers: one variable per number. Drag along an axis to zoom, right-click a plot to reset it. V1 / V2 / H1 / H2 place cursors to measure. The gear button sets each variable's name, unit, color and gain. Plot windows can be moved and resized; the Commands window keeps the commands you send often.", "Chaque ligne reçue est découpée en nombres : une variable par nombre. Glissez le long d'un axe pour zoomer, clic droit sur un graphique pour revenir. V1 / V2 / H1 / H2 placent des curseurs pour mesurer. Le bouton engrenage règle le nom, l'unité, la couleur et le gain de chaque variable. Les fenêtres de graphiques se déplacent et se redimensionnent; la fenêtre Commandes garde les commandes envoyées souvent."},
  {"Records tests to compare them. Check the film button of a command in the Commands window: sending it starts a test. A test ends when its condition is met (Settled: the watched variable stays near the constant; or a custom condition), after the max test time, or when Auto stable sees it stopped changing. Tests are overlaid on up to 4 graphs; they can be saved as CSV files and opened again.", "Enregistre des essais pour les comparer. Cochez le bouton film d'une commande dans la fenêtre Commandes : l'envoyer démarre un essai. Un essai se termine quand sa condition est remplie (Stabilisé : la variable surveillée reste près de la constante; ou une condition personnalisée), après la durée max, ou quand Stabilité auto voit qu'elle ne change plus. Les essais se superposent sur jusqu'à 4 graphiques; ils s'enregistrent en CSV et se rouvrent."},
  {"Runs tests one after the other: each row's commands (separated by |) start a test, the next row is sent when it ends. Prepare commands bring the setup back before each test; Start when waits until a condition holds (ex: the temperature is back) before the next one.", "Fait des essais l'un après l'autre : les commandes de chaque rangée (séparées par |) démarrent un essai, la rangée suivante est envoyée quand il se termine. Les commandes Préparer remettent le montage en place avant chaque essai; Démarrer quand attend qu'une condition soit vraie (ex : la température est revenue) avant le suivant."},
  {"Auto save writes each test that ends to the folder: CSV file and picture of the graphs. Report pictures are on white, at the format and resolution chosen, ready for a lab report. Notify shows a Windows notification when a test or a chain ends by itself.", "Sauvegarde auto écrit chaque essai terminé dans le dossier : fichier CSV et image des graphiques. Les images Rapport sont sur fond blanc, au format et à la résolution choisis, prêtes pour un rapport de laboratoire. Notifier affiche une notification Windows quand un essai ou une chaîne se termine de lui-même."},
  {"The gear at the top right: language, size of the interface, theme and colors. Profiles keep a whole setup (port, variables, plots, commands, tests) under a name, to switch between projects.", "L'engrenage en haut à droite : langue, taille de l'interface, thème et couleurs. Les profils gardent toute une configuration (port, variables, graphiques, commandes, essais) sous un nom, pour passer d'un projet à l'autre."},
  {"Profiles", "Profils"},
  {"Profile", "Profil"},
  {"Load", "Charger"},
  {"Save as", "Enregistrer sous"},
  {"Profile name", "Nom du profil"},
  {LV_SYMBOL_SAVE " Save", LV_SYMBOL_SAVE " Enregistrer"},
  {"(none saved)", "(aucun enregistré)"},
  {"Last profile saved / loaded: %s. Loading a profile restarts the app.",
   "Dernier profil enregistré / chargé : %s. Charger un profil redémarre l'application."},
  {"A profile keeps the port, data format, variables, plots and commands. Loading a profile restarts the app.",
   "Un profil garde le port, le format des données, les variables, les graphiques et les commandes. "
   "Charger un profil redémarre l'application."},
  {"Enter a name (letters, digits, space, - _ .), without a space at the start or end.",
   "Entrez un nom (lettres, chiffres, espace, - _ .), sans espace au début ni à la fin."},
  {"Could not save \"%s\".", "Impossible d'enregistrer « %s »."},
  {"Saved \"%s\".", "« %s » enregistré."},
  {"Could not load \"%s\".", "Impossible de charger « %s »."},
  {"\"%s\" loaded: close and start the app again to use it.",
   "« %s » chargé : fermez et relancez l'application pour l'utiliser."},
  {"Click " LV_SYMBOL_TRASH " again to delete \"%s\".", "Cliquez encore sur " LV_SYMBOL_TRASH " pour supprimer « %s »."},
  {"Deleted \"%s\".", "« %s » supprimé."},
  {"Could not delete \"%s\".", "Impossible de supprimer « %s »."},
  {"Display", "Affichage"},
  {"Language", "Langue"},
  {"The language changes at the next start.", "La langue change au prochain démarrage."},
  {"Restart now", "Redémarrer"},
  {"UI scale", "Taille de l'interface"},
  {"The new scale applies at the next start.", "La nouvelle taille s'applique au prochain démarrage."},
  {"Max frame rate", "Images/s max"},
  {"15 fps\n30 fps\n60 fps\n120 fps", "15 i/s\n30 i/s\n60 i/s\n120 i/s"},
  {"Colors", "Couleurs"},
  {"Theme", "Thème"},
  {"Light\nDark", "Clair\nSombre"},
  {"Accent color", "Couleur d'accent"},
  {"Blue\nTeal\nGreen\nOrange\nRed\nPurple\nPink", "Bleu\nSarcelle\nVert\nOrange\nRouge\nViolet\nRose"},

  // Serial tab
  {"Connect", "Connecter"},
  {"Disconnect", "Déconnecter"},
  {"Cancel", "Annuler"},
  {"Connections", "Connexions"},
  {"Each connection plots up to %lu variables (C1: Var 1-%lu, C2: Var %lu-%lu...).",
   "Chaque connexion trace jusqu'à %lu variables (C1 : Var 1-%lu, C2 : Var %lu-%lu...)."},
  {"Baud rate", "Débit (bauds)"},
  {"None\nOdd\nEven\nMark\nSpace", "Aucune\nImpaire\nPaire\nMarque\nEspace"},
  {"1 stop\n1.5 stop\n2 stop", "1 arrêt\n1,5 arrêt\n2 arrêts"},
  {"Auto reconnect", "Reconnexion auto"},
  {"Data format", "Format des données"},
  {"Separator", "Séparateur"},
  {"Auto\nSpace\nComma\nSemicolon\nColon\nTab", "Auto\nEspace\nVirgule\nPoint-virgule\nDeux-points\nTabulation"},
  {"Line ending", "Fin de ligne"},
  {"None\nLF (\\n)\nCR (\\r)\nCR+LF (\\r\\n)", "Aucune\nLF (\\n)\nCR (\\r)\nCR+LF (\\r\\n)"},
  {"Received data will appear here", "Les données reçues s'afficheront ici"},
  {"Type, then Enter", "Écrivez, puis Entrée"},
  {"Send to C%lu, Enter", "Envoyer à C%lu, Entrée"},
  {"Text\nHex\nDec", "Texte\nHex\nDéc"},
  {"No port found", "Aucun port trouvé"},
  {"Plug in a VCOM / USB serial device.", "Branchez un appareil série VCOM / USB."},
  {"Enter a baud rate.", "Entrez un débit."},
  {"Invalid format: 1.5 stop bits needs 5 data bits,\n2 stop bits needs 6 to 8 data bits.",
   "Format invalide : 1,5 bit d'arrêt demande 5 bits de données,\n2 bits d'arrêt demandent 6 à 8 bits de données."},
  {"%s is already used by C%ld.", "%s est déjà utilisé par C%ld."},
  {"Cannot open %s.\nIs it used by another program?", "Impossible d'ouvrir %s.\nEst-il utilisé par un autre programme ?"},
  {"Disconnected", "Déconnecté"},
  {"Waiting for %s...", "En attente de %s..."},
  {"[Connected to %s %lu %s]\n", "[Connecté à %s %lu %s]\n"},
  {"[Disconnected]\n", "[Déconnecté]\n"},
  {"[%s disconnected, waiting to reconnect]\n", "[%s déconnecté, en attente de reconnexion]\n"},
  {"[%s disconnected]\n", "[%s déconnecté]\n"},
  {"[C%lu is not used]\n", "[C%lu n'est pas utilisée]\n"},
  {"[Not connected]\n", "[Non connecté]\n"},
  {"[Send failed]\n", "[Échec de l'envoi]\n"},

  // Plot tab
  {"Waiting for lines of numbers\nex: 17.08 17.50 -2.38", "En attente de lignes de nombres\nex. : 17.08 17.50 -2.38"},
  {"No variable in P%u\n" LV_SYMBOL_SETTINGS " > Show in", "Aucune variable dans P%u\n" LV_SYMBOL_SETTINGS " > Afficher dans"},
  {"No data to export.", "Aucune donnée à exporter."},
  {"Not enough memory to export.", "Mémoire insuffisante pour l'export."},
  {"Could not start the export.", "Impossible de lancer l'export."},
  {"Plots", "Graphiques"},
  {"Tile", "Mosaïque"},
  {"Length", "Longueur"},
  {"Commands", "Commandes"},
  {"Plot settings", "Paramètres du graphique"},
  {"All plots", "Tous les graphiques"},
  {"Commands window", "Fenêtre Commandes"},
  {"Show", "Afficher"},
  {"Y scale", "Échelle Y"},
  {"Min / max", "Min / max"},
  {"Line width", "Épaisseur du trait"},
  {"Scaling", "Mise à l'échelle"},
  {"X axis", "Axe X"},
  {"Samples\nSeconds\nClock time", "Échantillons\nSecondes\nHeure"},
  {"Sample number, seconds before the latest sample, or time of day",
   "Numéro d'échantillon, secondes avant le dernier échantillon, ou heure"},
  {"Plotted value = received value x gain + offset.", "Valeur tracée = valeur reçue x gain + décalage."},
  {"Chart", "Courbe"},
  {"Bars", "Barres"},
  {"ex: -10", "ex. : -10"},
  {"ex: 100", "ex. : 100"},
  {"Variable settings", "Paramètres de la variable"},
  {"Name", "Nom"},
  {"Unit", "Unité"},
  {"ex: V, mA, rpm", "ex. : V, mA, tr/min"},
  {"None\n°C\n°F\n%\nV\nmV\nA\nmA\nW\nHz\nrpm\ns\nms\nm\nmm\nkg\ng\nN\nPa\nbar",
   "Aucune\n°C\n°F\n%\nV\nmV\nA\nmA\nW\nHz\ntr/min\ns\nms\nm\nmm\nkg\ng\nN\nPa\nbar"},
  {"Color", "Couleur"},
  {"Default\nBlue\nRed\nGreen\nOrange\nPurple\nCyan\nPink\nGray\nLime\nSky\nRose\nYellow\nIndigo\nTeal\nFuchsia\nBrown",
   "Défaut\nBleu\nRouge\nVert\nOrange\nViolet\nCyan\nRose\nGris\nLime\nCiel\nFramboise\nJaune\nIndigo\nSarcelle\nFuchsia\nBrun"},
  {"ex: 2.5 or /4", "ex. : 2.5 ou /4"},
  {"Offset", "Décalage"},
  {"ex: -10.5", "ex. : -10.5"},
  {"Thickness", "Épaisseur"},
  {"Show in", "Afficher dans"},
  {"Plotted value = received value x gain + offset.\n"
   "Plots: P1 to P8 are shown according to \"Plots\".\n"
   "Zoom: drag a box with the Zoom tool, or drag along an axis.",
   "Valeur tracée = valeur reçue x gain + décalage.\n"
   "Graphiques : P1 à P8 sont affichés selon « Graphiques ».\n"
   "Zoom : tracez un cadre avec l'outil Zoom, ou glissez le long d'un axe."},

  // Commands window
  {"Command", "Commande"},
  {"Sep", "Sép"},
  {"Values: a,b,c", "Valeurs : a,b,c"},
  {"Starting text", "Texte initial"},
  {"Value", "Valeur"},
  {"Max 32 commands", "32 commandes max"},

  // Tooltips
  {"Serial port (VCOM / USB)", "Port série (VCOM / USB)"},
  {"Refresh the port list", "Actualiser la liste des ports"},
  {"Open / close the selected port", "Ouvrir / fermer le port choisi"},
  {"Show / hide the connection settings", "Afficher / masquer les paramètres de connexion"},
  {"Common baud rates", "Débits courants"},
  {"Connection shown below (port, settings, sent text). Green: connected, amber: waiting to reconnect.",
   "Connexion affichée ci-dessous (port, paramètres, texte envoyé). Vert : connectée, orange : en attente de reconnexion."},
  {"Number of serial ports used at once; they share the 32 variables",
   "Nombre de ports série utilisés en même temps; ils se partagent les 32 variables"},
  {"Reopen the port when it comes back (unplugged / at startup)",
   "Rouvrir le port quand il revient (débranché / au démarrage)"},
  {"Character between the values of a received line", "Caractère entre les valeurs d'une ligne reçue"},
  {"Values plotted per line (Auto: as many as received)", "Valeurs tracées par ligne (Auto : autant que reçues)"},
  {"Added at the end of each sent line", "Ajoutée à la fin de chaque ligne envoyée"},
  {"How the received bytes are shown in the terminal", "Affichage des octets reçus dans le terminal"},
  {"Send (Enter)", "Envoyer (Entrée)"},
  {"Clear the terminal", "Effacer le terminal"},
  {"Load the selected profile (restarts the app)", "Charger le profil choisi (redémarre l'application)"},
  {"Delete the selected profile", "Supprimer le profil choisi"},
  {"Save the current settings under this name", "Enregistrer les paramètres actuels sous ce nom"},
  {"Drag on a plot: V1 / V2 vertical cursors, H1 / H2 horizontal cursors, Zoom: zoom box",
   "Glisser sur un graphique : V1 / V2 curseurs verticaux, H1 / H2 curseurs horizontaux, Zoom : cadre de zoom"},
  {"Remove the cursors", "Retirer les curseurs"},
  {"Reset the zoom of all plots (right-click a plot: that plot only)",
   "Annuler le zoom de tous les graphiques (clic droit sur un graphique : celui-là seulement)"},
  {"Pause / resume the plots", "Mettre en pause / reprendre les graphiques"},
  {"Clear the plotted data", "Effacer les données tracées"},
  {"Export the data to a CSV file", "Exporter les données dans un fichier CSV"},
  {"Plot and variable settings", "Paramètres des graphiques et des variables"},
  {"Drag to move. Right-click the plot to reset its zoom.", "Glisser pour déplacer. Clic droit sur le graphique pour annuler son zoom."},
  {"Drag to move", "Glisser pour déplacer"},
  {"Drag to resize", "Glisser pour redimensionner"},
  {"Drag to share the width between the chart and the bars", "Glisser pour partager la largeur entre la courbe et les barres"},
  {"Drag along the axis to zoom", "Glisser le long de l'axe pour zoomer"},
  {"Arrange the windows side by side", "Placer les fenêtres côte à côte"},
  {"Samples shown by the plots", "Échantillons affichés par les graphiques"},
  {"Show the Commands window", "Afficher la fenêtre Commandes"},
  {"Marks", "Repères"},
  {"Line on the plot when a command is sent, with the command", "Ligne sur le graphique à l'envoi d'une commande, avec la commande"},
  {"Line chart (off: bar graph only)", "Courbe (désactivée : barres seulement)"},
  {"Bar graph of the current values", "Graphique à barres des valeurs actuelles"},
  {"Y scale follows the data (off: Y min / Y max)", "L'échelle Y suit les données (désactivée : Y min / Y max)"},
  {"Common units", "Unités courantes"},
  {"Preset colors", "Couleurs prédéfinies"},
  {"Plotted value = received value x gain (\"/4\": divided by 4)", "Valeur tracée = valeur reçue x gain (« /4 » : divisée par 4)"},
  {"Plots where this variable is shown", "Graphiques où cette variable est affichée"},
  {"Close", "Fermer"},
  {"Text field: the value is typed on the command's row", "Champ texte : la valeur est tapée sur la ligne de la commande"},
  {"Connection the command is sent on", "Connexion sur laquelle la commande est envoyée"},
  {"Add the command / save the changes", "Ajouter la commande / enregistrer les modifications"},
  {"Drag to reorder", "Glisser pour réordonner"},
  {"Edit", "Modifier"},
  {"Delete", "Supprimer"},
  {"Send this command", "Envoyer cette commande"},

  // Export
  {"Export plot data", "Exporter les données"},
  {"Exported %lu samples to %s", "%lu échantillons exportés dans %s"},
  {"Could not write %s", "Impossible d'écrire %s"},
  {"Sample,Date time,Time (s)", "Échantillon,Date heure,Temps (s)"},

  // Compare tab
  {"Test bench", "Banc d'essai"},
  {"Picture", "Image"},
  {"As on screen\nReport", "Comme à l'écran\nRapport"},
  {"A report picture is on white, with text and lines sized for the page.", "Une image de rapport est sur fond blanc, avec le texte et les lignes adaptés à la page."},
  {"Format", "Format"},
  {"Width : height of the pictures, ex: 16:8", "Largeur : hauteur des images, ex : 16:8"},
  {"Page width", "Largeur page"},
  {"Width of the picture on the page", "Largeur de l'image sur la page"},
  {"Resolution", "Résolution"},
  {"Pixels per inch: 300 for a printed report", "Pixels par pouce : 300 pour un rapport imprimé"},
  {"Format: two numbers, ex: 16:8", "Format : deux nombres, ex : 16:8"},
  {"Pictures of %ld x %ld pixels", "Images de %ld x %ld pixels"},
  {"Auto save", "Sauvegarde auto"},
  {"Notify", "Notifier"},
  {"Windows notification when a test or a chain ends by itself, not when you stop it", "Notification Windows quand un essai ou une chaîne se termine de lui-même, pas quand vous l'arrêtez"},
  {"Folder", "Dossier"},
  {"Each test that ends is saved in the folder: CSV file and picture of the graphs", "Chaque essai terminé est enregistré dans le dossier : fichier CSV et image des graphiques"},
  {"Choose the folder", "Choisir le dossier"},
  {"Folder of the test bench", "Dossier du banc d'essai"},
  {"Could not create the test bench folder.", "Impossible de créer le dossier du banc d'essai."},
  {"Could not write a CSV file of the test bench.", "Impossible d'écrire un fichier CSV du banc d'essai."},
  {"Picture saved: %s", "Image enregistrée : %s"},
  {"[%s: receive queue full, reading paused %lu ms (data kept; the device may have been held back)]\n",
   "[%s : file de réception pleine, lecture suspendue %lu ms (données conservées ; l'appareil a pu être retenu)]\n"},
  {"[%s: Windows reports received data lost before the app could read it]\n",
   "[%s : Windows signale des données reçues perdues avant leur lecture par l'application]\n"},
  {"[%s: framing / parity errors reported by Windows]\n", "[%s : erreurs de trame / parité signalées par Windows]\n"},
  {"[%s: %lu%% of the receive queue waiting (the app is behind the data)]\n",
   "[%s : %lu %% de la file de réception en attente (l'application est en retard sur les données)]\n"},
  {"Save a picture of the graphs in the test bench folder", "Enregistrer une image des graphiques dans le dossier du banc d'essai"},
  {"Save a picture of the plots in the test bench folder", "Enregistrer une image des graphiques dans le dossier du banc d'essai"},
  {"Reset the zoom of all graphs (right-click a graph: that graph only). Drag on a graph to zoom.", "Réinitialiser le zoom de tous les graphiques (clic droit sur un graphique : celui-ci seulement). Glisser sur un graphique pour zoomer."},
  {"Number of graphs. The buttons of a test or a variable choose its graphs.", "Nombre de graphiques. Les boutons d'un essai ou d'une variable choisissent ses graphiques."},
  {"Graphs showing this test", "Graphiques qui montrent cet essai"},
  {"%s (constant)", "%s (constante)"},
  {"4 variables at most per graph.", "4 variables au maximum par graphique."},
  {"Put a test and a variable on this graph with the number buttons at the left.", "Placez un essai et une variable sur ce graphique avec les boutons numérotés à gauche."},
  {"Max test time", "Durée max"},
  {"Longest time for a test, in seconds (0: no limit). It then ends as not stable, and a chain goes on.", "Durée maximale d'un essai, en secondes (0 : sans limite). Il se termine alors comme non stable, et une chaîne continue."},
  {"Start when", "Démarrer quand"},
  {"Held for", "Maintenu"},
  {"Each row waits for this condition (empty: no wait). Ex: watch>=20, or abs(watch-start)<1 to come back near the value when the last test started.", "Chaque ligne attend cette condition (vide : aucune attente). Ex : watch>=20, ou abs(watch-start)<1 pour revenir près de la valeur au début du dernier essai."},
  {"Seconds the Start when condition must stay true without a break", "Secondes pendant lesquelles la condition Démarrer quand doit rester vraie sans interruption"},
  {"Chain stopped: no variable received yet.", "Chaîne arrêtée : aucune variable reçue."},
  {"Chain stopped: the Start when condition is not valid.", "Chaîne arrêtée : la condition Démarrer quand n'est pas valide."},
  {"The Start when condition is not valid.", "La condition Démarrer quand n'est pas valide."},
  {"Waiting before test %lu of %lu: %s, true for %.1f of %.1f s", "Attente avant l'essai %lu sur %lu : %s, vraie depuis %.1f sur %.1f s"},
  {"Chain", "Chaîne"},
  {"Run the commands one after the other, each starting a test / stop", "Exécuter les commandes l'une après l'autre, chacune démarrant un essai / arrêter"},
  {"Add a command", "Ajouter une commande"},
  {"Move up", "Monter"},
  {"Test not stable after %.1f s: %s", "Essai non stable après %.1f s : %s"},
  {"Not stable after %.1f s, %lu samples", "Non stable après %.1f s, %lu échantillons"},
  {"Chain stopped.", "Chaîne arrêtée."},
  {"Chain stopped, could not send: %s", "Chaîne arrêtée, envoi impossible : %s"},
  {"Chain finished: %lu tests.", "Chaîne terminée : %lu essais."},
  {"Running: test %lu of %lu, %s", "En cours : essai %lu sur %lu, %s"},
  {"Between", "Entre"},
  {"Sent: %s", "Envoyé : %s"},
  {"Prepare", "Préparer"},
  {"Commands bringing the setup to the start point, separated by |. Sent after each test, however it ends, and before the first one (with a Start when condition).", "Commandes qui amènent le montage au point de départ, séparées par |. Envoyées après chaque essai, peu importe comment il se termine, et avant le premier (avec une condition Démarrer quand)."},
  {"Chain stopped, the setup is not ready, could not send: %s", "Chaîne arrêtée, le montage n'est pas prêt, envoi impossible : %s"},
  {"Wait for the Prepare commands to be sent.", "Attendez l'envoi des commandes Préparer."},
  {"Time between the commands of a test", "Temps entre les commandes d'un essai"},
  {"Commands of this test, separated by | (ex: Kp:2 | SP:5). The last one starts the test.", "Commandes de cet essai, séparées par | (ex : Kp:2 | SP:5). La dernière démarre l'essai."},
  {"Each row is a test, its commands separated by |. The next row is sent when it ends.", "Chaque ligne est un essai, ses commandes séparées par |. La ligne suivante est envoyée quand il se termine."},
  {"Add commands to the chain first.", "Ajoutez d'abord des commandes à la chaîne."},
  {"Stop the test before starting the chain.", "Arrêtez l'essai avant de démarrer la chaîne."},
  {"Not enough room for the tests of the chain: delete some tests.", "Pas assez de place pour les essais de la chaîne : supprimez des essais."},
  {"A chain needs a valid condition, Auto stable or a max test time.", "Une chaîne demande une condition valide, Stabilité auto ou une durée max."},
  {"Auto stable", "Stabilité auto"},
  {"Device settings", "Réglages de l'appareil"},
  {"Request", "Demande"},
  {"Reply starts", "Début réponse"},
  {"Command asking the device for its settings, sent when each test starts (empty: none)", "Commande qui demande ses réglages à l'appareil, envoyée au début de chaque essai (vide : aucune)"},
  {"Text at the start of the device's reply: such lines are the settings of the test, not data. Needed to tell them from the data.", "Texte au début de la réponse de l'appareil : ces lignes sont les réglages de l'essai, pas des données. Nécessaire pour les distinguer des données."},
  {"Could not send the settings request.", "Impossible d'envoyer la demande de réglages."},
  {"No reply to the settings request: check the Reply starts text.", "Aucune réponse à la demande de réglages : vérifiez le texte Début réponse."},
  {"Also ends the test when the watched variable stops changing, even far from the constant. An oscillation keeps it going.", "Termine aussi l'essai quand la variable surveillée cesse de changer, même loin de la constante. Une oscillation le fait continuer."},
  {"Smooth", "Lissage"},
  {"Averaged over this time for Auto stable: longer than the noise, less than half the period of the oscillations that must keep a test going", "Moyenne sur ce temps pour Stabilité auto : plus long que le bruit, moins de la moitié de la période des oscillations qui doivent faire continuer l'essai"},
  {"Time over which the 3 highest and 3 lowest peaks must be within the tolerance. Use more than the slowest part of the response.", "Temps pendant lequel les 3 pics les plus hauts et les 3 plus bas doivent être dans la tolérance. Utilisez plus que la partie la plus lente de la réponse."},
  {"Also when the peaks of %s, averaged over %.4g s, are within %.4g for %.4g s.", "Aussi quand les pics de %s, moyennée sur %.4g s, sont à moins de %.4g pendant %.4g s."},
  {"Test stopped changing at %.2f after %.1f s: %s", "Essai stable à %.2f après %.1f s : %s"},
  {"Stable at %.2f from %.1f s, %lu samples", "Stable à %.2f depuis %.1f s, %lu échantillons"},
  {"Compare", "Comparer"},
  {"Recording", "Enregistrement"},
  {"Constant", "Constante"},
  {"Watched", "Surveillée"},
  {"Stop when", "Arrêter quand"},
  {"Settled\nCustom condition", "Stabilisé\nCondition personnalisée"},
  {"Last samples", "Derniers éch."},
  {"Within", "À moins de"},
  {"1 unit =", "1 unité ="},
  {"Tests", "Essais"},
  {"Compared", "Comparées"},
  {"None", "Aucune"},
  {"Manual", "Manuel"},
  {"Open tests", "Ouvrir des essais"},
  {"Start a test now / stop the test. Commands with this symbol checked start one when sent.", "Démarrer un essai maintenant / arrêter l'essai. Les commandes avec ce symbole coché en démarrent un à l'envoi."},
  {"Same in every test (ex: the setpoint): drawn once. Handle: ref", "Identique dans chaque essai (ex : la consigne) : tracée une seule fois. Nom : ref"},
  {"Variable that must reach the constant (ex: the temperature). Handle: watch", "Variable qui doit atteindre la constante (ex : la température). Nom : watch"},
  {"How many samples in a row must be within the tolerance. Handle: N", "Nombre d'échantillons de suite qui doivent être dans la tolérance. Nom : N"},
  {"Tolerance around the constant, in percent", "Tolérance autour de la constante, en pourcentage"},
  {"Percent for 1 unit of the variable. Ex: 1 \xC2\xB0" "C = 2.5 %, then 5 % is 2 \xC2\xB0" "C. Handle: band", "Pourcentage pour 1 unité de la variable. Ex : 1 \xC2\xB0" "C = 2,5 %, alors 5 % vaut 2 \xC2\xB0" "C. Nom : band"},
  {"Add the data of the plots as a test", "Ajouter les données des graphiques comme essai"},
  {"Open tests from CSV files of the export", "Ouvrir des essais depuis des fichiers CSV exportés"},
  {"Reset the zoom (or right-click the chart). Drag on the chart to zoom.", "Réinitialiser le zoom (ou clic droit sur le graphique). Glisser sur le graphique pour zoomer."},
  {"Test not recorded: 16 tests at most, delete one in the Compare tab.", "Essai non enregistré : 16 essais au maximum, supprimez-en un dans l'onglet Comparer."},
  {"Test not recorded: no variable received yet.", "Essai non enregistré : aucune variable reçue."},
  {"Test not recorded: not enough memory.", "Essai non enregistré : mémoire insuffisante."},
  {"Test started: %s", "Essai démarré : %s"},
  {"Test started, stop it in the Compare tab: %s", "Essai démarré, arrêtez-le dans l'onglet Comparer : %s"},
  {"Test ended (condition met) after %.1f s: %s", "Essai terminé (condition atteinte) après %.1f s : %s"},
  {"Test ended, its memory is full, after %.1f s: %s", "Essai terminé, mémoire pleine, après %.1f s : %s"},
  {"Test stopped after %.1f s: %s", "Essai arrêté après %.1f s : %s"},
  {"Stops with the Stop button: no condition.", "S'arrête avec le bouton Arrêt : aucune condition."},
  {"Choose the Constant and Watched variables to stop automatically.", "Choisissez les variables Constante et Surveillée pour arrêter automatiquement."},
  {"The percent for 1 unit must be above 0.", "Le pourcentage pour 1 unité doit être supérieur à 0."},
  {"At least 1 sample is needed.", "Il faut au moins 1 échantillon."},
  {"Checked when the variables are received.", "Vérifiée quand les variables sont reçues."},
  {"Unknown name at character %lu.", "Nom inconnu au caractère %lu."},
  {"A function over samples cannot be inside another (character %lu).", "Une fonction sur des échantillons ne peut pas être dans une autre (caractère %lu)."},
  {"Condition too long.", "Condition trop longue."},
  {"Syntax error at character %lu.", "Erreur de syntaxe au caractère %lu."},
  {"The test ends when the condition is true.", "L'essai se termine quand la condition est vraie."},
  {"Ends when %s stays within %.4g of %s for %lu samples.", "Se termine quand %s reste à moins de %.4g de %s pendant %lu échantillons."},
  {"Recording: %s, %.1f s, %lu samples", "Enregistrement : %s, %.1f s, %lu échantillons"},
  {"Recording, %.1f s", "Enregistrement, %.1f s"},
  {"Reached at %.1f s, %lu samples", "Atteint à %.1f s, %lu échantillons"},
  {"%.1f s, %lu samples", "%.1f s, %lu échantillons"},
  {"Name of the test (ex: its PID gains)", "Nom de l'essai (ex : ses gains PID)"},
  {"Save this test to a CSV file", "Enregistrer cet essai dans un fichier CSV"},
  {"%s differs between the tests (%.2f to %.2f): the gray line is the longest test's.", "%s diffère entre les essais (%.2f à %.2f) : la ligne grise est celle de l'essai le plus long."},
  {"Stop the test before saving it.", "Arrêtez l'essai avant de l'enregistrer."},
  {"16 tests at most: delete one first.", "16 essais au maximum : supprimez-en un d'abord."},
  {"Could not open the files.", "Impossible d'ouvrir les fichiers."},
  {"%lu tests opened, %lu not (not an export file, or 16 tests at most).", "%lu essais ouverts, %lu non (pas un fichier d'export, ou 16 essais au maximum)."},
  {"Sending this command starts a test (Compare tab)", "L'envoi de cette commande démarre un essai (onglet Comparer)"},
  {"Waiting: send a command with " LV_SYMBOL_VIDEO " checked, or press " LV_SYMBOL_VIDEO ".",
   "En attente : envoyez une commande avec " LV_SYMBOL_VIDEO " coché, ou appuyez sur " LV_SYMBOL_VIDEO "."},
  {"No test yet.\nCheck " LV_SYMBOL_VIDEO " on a command in the Commands window, then send it.",
   "Aucun essai.\nCochez " LV_SYMBOL_VIDEO " sur une commande dans la fenêtre Commandes, puis envoyez-la."},
  {"Variables: Temp, \"Var 1\". Handles: ref, watch, band, N, t (s), n.\n"
   "Functions: abs, min, max. Last k samples: held(cond, k), avg(x, k), "
   "lowest(x, k), highest(x, k), prev(x, k), slope(x, k).\n"
   "Ex: held(abs(watch-ref)<0.5,N) || t>600",
   "Variables : Temp, \"Var 1\". Noms : ref, watch, band, N, t (s), n.\n"
   "Fonctions : abs, min, max. k derniers échantillons : held(cond, k), avg(x, k), "
   "lowest(x, k), highest(x, k), prev(x, k), slope(x, k).\n"
   "Ex : held(abs(watch-ref)<0.5,N) || t>600"},
};

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void langSet(Lang_t lang)
{
  language = (lang < LANG_COUNT) ? lang : LANG_EN;
}

Lang_t langGet(void)
{
  return language;
}

// Used when the UI is created / a message is shown: a linear search is fast enough
const char * tr(const char * text)
{
  if (language == LANG_EN)
  {
    return text;
  }

  for (uint32_t i = 0; i < (sizeof(translations) / sizeof(translations[0])); ++i)
  {
    if (strcmp(translations[i].en, text) == 0)
    {
      return translations[i].fr;
    }
  }

  return text;
}
