# Notes de version de CyModelHub

[English](VERSION.md)

## 0.1.0 — Première version fonctionnelle

**EN DÉVELOPPEMENT.** Compilé et testé sous Windows 10 x64 avec MSVC 19.40.

- Service local avec catalogue persistant, démarré à la demande, joignable sur la boucle
  locale avec un jeton d'accès.
- Découverte automatique du cache Hugging Face et des dépôts locaux d'Ollama et de LM Studio,
  plus tout dossier surveillé ; ajout manuel d'un fichier ou d'un dossier de modèle.
- Classement par rôle, domaine et famille à partir des en-têtes GGUF et safetensors, des
  dossiers Diffusers, Transformers et MLX, puis des noms de dossiers et de fichiers. Les
  corrections manuelles sont conservées.
- Résolution d'un modèle par nom, nom de fichier, dépôt, SHA-256 ou identifiant.
- Étiquettes, notes, favoris, doublons, SHA-256 à la demande.
- Déplacement d'un modèle (y compris d'un disque à l'autre), liens physiques et symboliques
  vers d'autres dossiers.
- Installation depuis Hugging Face ou une adresse HTTPS, avec reprise et vérification du
  SHA-256.
- Bibliothèque cliente C++, outil en ligne de commande, MCP en HTTP et relais MCP stdio.
- La bibliothèque cliente est autonome : bibliothèque standard C++, système d'exploitation et
  une copie de nlohmann/json fournie avec le dépôt. L'intégrer dans une application ne
  télécharge rien.
- Installations partagées : les applications qui demandent le même modèle rejoignent une
  seule installation et obtiennent le même résultat ; chacune a un ticket, et le travail
  n'est annulé que lorsque le dernier demandeur s'en va. Une installation interrompue par un
  arrêt du service est reprise.
- Clés d'idempotence sur toute opération qui modifie quelque chose : une demande renvoyée
  après une réponse perdue n'a pas de second effet ; la même clé avec d'autres arguments est
  un conflit. Conservées 7 jours, y compris après un redémarrage. Le client ne répète plus
  une écriture qui ne porte pas de clé.
- Modèles complets : un contrat liste chaque fichier nécessaire à un modèle, avec tailles et
  SHA-256. Un modèle est installé dans un dossier provisoire et publié seulement quand chaque
  fichier a passé son contrôle, ou enregistré là où une application le possède déjà, sans
  copie ni déplacement.
- Résolution stricte (`models_check`, `Client::check`) : prêt, absent, en cours
  d'installation, incomplet, invalide, non vérifié, incompatible ou ambigu, jamais « la
  correspondance la plus proche ».
- Vraie reconnaissance des modèles de parole Fermion (`.fermion`) : l'index du conteneur est
  lu et contrôlé par rapport à sa taille, à son manifeste et à sa configuration.
- Application de bureau construite avec CyUI, en français et en anglais.
- Icône dans la zone de notification Windows pour rouvrir la fenêtre, accéder aux réglages
  ou quitter l’application. La croix masque la fenêtre.
- Manuel CyDoc hors ligne en anglais et en français, accessible depuis les réglages ou avec **F1**.
- Licences : GPL-3.0-only, sauf la bibliothèque cliente, sous MIT pour que toute application
  puisse la lier.

### Limites connues

- **Linux et macOS n'ont été ni compilés ni exécutés.** Le code évite les appels propres à
  une plateforme hors de `src/platform.cpp`, mais seul Windows a été vérifié.
- L'application a été contrôlée sur ses écrans (modèles, emplacements, doublons, activité,
  dialogues) dans les deux langues ; chaque bouton déclenche une opération du service
  couverte par les tests, mais les clics eux-mêmes ne sont pas exercés par un test
  automatique.
- La détection est heuristique. Les fichiers safetensors à la structure inhabituelle, les
  fichiers pickle et les modèles ONNX ne sont classés que d'après leurs noms de dossier et de
  fichier.
- Les messages qui viennent du service (erreurs, titres des tâches) sont en anglais.
- Le service ne démarre pas avec la session ; il démarre quand une application en a besoin.
- L'installation depuis Hugging Face a été testée contre un serveur local qui imite sa
  redirection et son en-tête d'empreinte, pas contre huggingface.co.
- Pas de suppression de fichiers de modèles, pas de remplacement automatique des doublons
  par des liens, pas encore de recherche Civitai.
- Un fichier endommagé d'un modèle installé est signalé, jamais réparé : quelqu'un doit le
  remplacer. Les dossiers provisoires et les fichiers partiels des installations qui ne se
  terminent jamais restent en place.
- Modèles complets : les fichiers d'un dépôt ne sont pas découverts (l'appelant les liste),
  les dépendances sont enregistrées mais pas installées, et les archives de téléchargement
  de Fermion (`.fsq`, `.bps.tar.zst`) ne sont pas décompressées : l'application décompresse,
  puis enregistre le dossier.
- Les tests des installations partagées n'utilisent que des fixtures et un serveur local.
  L'installation d'un modèle complet depuis huggingface.co lui-même n'a pas été exercée.
- L'application de bureau n'affiche pas encore l'état d'un modèle (prêt, incomplet, invalide).
- Voir [docs/INTEGRATION.fr.md](docs/INTEGRATION.fr.md) pour les limites du parcours
  d'intégration.
