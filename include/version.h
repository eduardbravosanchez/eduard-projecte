/**
 * @file version.h
 * @brief Definició de la versió del firmware del sistema i metadades de compilació.
 * @project D.05 — Despertador amb Ràdio FM Real
 * @author Eduard Bravo Sánchez · Processadors Digitals · UPC
 */

#ifndef VERSION_H
#define VERSION_H

// SemVer (Semantic Versioning)
#define FIRMWARE_VERSION_MAJOR   1  ///< Incrementa amb canvis estructurals grans
#define FIRMWARE_VERSION_MINOR   0  ///< Incrementa amb noves funcions (ex. afegir Wi-Fi)
#define FIRMWARE_VERSION_PATCH   0  ///< Incrementa amb correcció d'errors (bugfixes)

// Cadena de text per mostrar per pantalla o Serial
#define FIRMWARE_VERSION_STR    "1.0.0"

// Metadades automàtiques del compilador (agafen l'hora exacta de la teva màquina en compilar)
#define FIRMWARE_BUILD_DATE     __DATE__
#define FIRMWARE_BUILD_TIME     __TIME__

#endif // VERSION_H