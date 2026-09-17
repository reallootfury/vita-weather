#include "i18n.h"

#include <stdio.h>
#include <string.h>

static const char *const g_text[APP_LANGUAGE_COUNT][TEXT_COUNT] = {
    [APP_LANGUAGE_ENGLISH] = {
        "Now", "Hours", "10-day", "Map", "Right now", "This week",
        "High / Low", "Feels like", "At a glance", "Humidity", "Wind",
        "Rain chance", "Sunset", "Hourly forecast", "Temperature", "Rain",
        "10-day forecast", "Today", "Refresh", "Locations", "Settings",
        "Pages", "Select", "Search", "Remove", "Back", "Use current location",
        "Saved locations", "No saved locations yet",
        "GPS works on compatible Vita 1000 models", "Time format", "Language",
        "Appearance", "Effect style", "Realistic", "Pixel", "Weather animation",
        "Weather sounds", "Auto GPS", "Animated", "Gentle", "Static", "Off", "On",
        "Auto", "Day", "Night", "Find a location", "Type to see suggestions",
        "No matching locations", "Map layer", "Precipitation", "Air quality",
        "Tomorrow", "Details", "UV index", "Visibility", "Pressure",
        "Cloud cover", "Sunrise", "Scroll", "Map style", "Day details",
        "Units", "Metric", "Imperial", "GPS active", "GPS inactive - select to retry",
        "Hourly data unavailable", "Live", "Offline", "Sample", "Sync"
    },
    [APP_LANGUAGE_SPANISH] = {
        "Ahora", "Horas", "10 dias", "Mapa", "Ahora mismo", "Esta semana",
        "Max / Min", "Sensacion", "De un vistazo", "Humedad", "Viento",
        "Prob. lluvia", "Puesta", "Pronostico por hora", "Temperatura", "Lluvia",
        "Pronostico de 10 dias", "Hoy", "Actualizar", "Ubicaciones", "Ajustes",
        "Paginas", "Elegir", "Buscar", "Borrar", "Atras", "Usar ubicacion actual",
        "Ubicaciones guardadas", "Aun no hay ubicaciones guardadas",
        "El GPS funciona en modelos Vita 1000 compatibles", "Formato horario", "Idioma",
        "Apariencia", "Estilo de efectos", "Realista", "Pixel", "Animacion del tiempo",
        "Sonidos del tiempo", "GPS automatico", "Animado", "Suave", "Estatico", "No", "Si",
        "Auto", "Dia", "Noche", "Buscar una ubicacion", "Escribe para ver sugerencias",
        "No hay ubicaciones", "Capa del mapa", "Precipitacion", "Calidad del aire",
        "Manana", "Detalles", "Indice UV", "Visibilidad", "Presion",
        "Nubosidad", "Amanecer", "Desplazar", "Estilo de mapa", "Detalles del dia",
        "Unidades", "Metrico", "Imperial", "GPS activo", "GPS inactivo - elige para reintentar",
        "Datos horarios no disponibles", "En vivo", "Sin red", "Muestra", "Guardado"
    },
    [APP_LANGUAGE_FRENCH] = {
        "Maint.", "Heures", "10 jours", "Carte", "Maintenant", "Cette semaine",
        "Max / Min", "Ressenti", "En un coup d'oeil", "Humidite", "Vent",
        "Risque pluie", "Coucher", "Previsions horaires", "Temperature", "Pluie",
        "Previsions sur 10 jours", "Auj.", "Actualiser", "Lieux", "Reglages",
        "Pages", "Choisir", "Rechercher", "Supprimer", "Retour", "Utiliser ma position",
        "Lieux enregistres", "Aucun lieu enregistre",
        "Le GPS fonctionne sur les Vita 1000 compatibles", "Format de l'heure", "Langue",
        "Apparence", "Style d'effets", "Realiste", "Pixel", "Animation meteo",
        "Sons meteo", "GPS auto", "Animee", "Douce", "Statique", "Non", "Oui",
        "Auto", "Jour", "Nuit", "Rechercher un lieu", "Saisissez pour voir les suggestions",
        "Aucun lieu trouve", "Couche de carte", "Precipitations", "Qualite de l'air",
        "Demain", "Details", "Indice UV", "Visibilite", "Pression",
        "Nuages", "Lever", "Defiler", "Style de carte", "Details du jour",
        "Unites", "Metrique", "Imperial", "GPS actif", "GPS inactif - choisir pour reessayer",
        "Donnees horaires indisponibles", "Direct", "Hors ligne", "Exemple", "Synchro"
    },
    [APP_LANGUAGE_GERMAN] = {
        "Jetzt", "Stunden", "10 Tage", "Karte", "Gerade jetzt", "Diese Woche",
        "Hoch / Tief", "Gefuhlt", "Auf einen Blick", "Feuchte", "Wind",
        "Regenrisiko", "Sonnenunterg.", "Stundliche Vorhersage", "Temperatur", "Regen",
        "10-Tage-Vorhersage", "Heute", "Aktualisieren", "Orte", "Einstellungen",
        "Seiten", "Auswahlen", "Suchen", "Loschen", "Zuruck", "Aktuellen Ort verwenden",
        "Gespeicherte Orte", "Noch keine gespeicherten Orte",
        "GPS funktioniert auf kompatiblen Vita-1000-Modellen", "Zeitformat", "Sprache",
        "Darstellung", "Effektstil", "Realistisch", "Pixel", "Wetteranimation",
        "Wetterton", "Auto-GPS", "Animiert", "Sanft", "Statisch", "Aus", "Ein",
        "Auto", "Tag", "Nacht", "Ort suchen", "Tippen fur Vorschlage",
        "Keine passenden Orte", "Kartenebene", "Niederschlag", "Luftqualitat",
        "Morgen", "Details", "UV-Index", "Sichtweite", "Luftdruck",
        "Bewolkung", "Sonnenaufg.", "Scrollen", "Kartenstil", "Tagesdetails",
        "Einheiten", "Metrisch", "Imperial", "GPS aktiv", "GPS inaktiv - zum Wiederholen wahlen",
        "Keine Stundendaten verfugbar", "Live", "Offline", "Beispiel", "Sync"
    }
};

const char *tr(AppLanguage language, TextKey key)
{
    if ((unsigned int)language >= APP_LANGUAGE_COUNT)
        language = APP_LANGUAGE_ENGLISH;
    if ((unsigned int)key >= TEXT_COUNT)
        return "";
    return g_text[language][key];
}

const char *tr_day_label(AppLanguage language, const char *label)
{
    static const char *const source[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const labels[APP_LANGUAGE_COUNT][7] = {
        {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
        {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"},
        {"Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam"},
        {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"}
    };
    if ((unsigned int)language >= APP_LANGUAGE_COUNT) language = APP_LANGUAGE_ENGLISH;
    for (int i = 0; i < 7; ++i)
        if (label && !strcmp(label, source[i])) return labels[language][i];
    return label ? label : "";
}

const char *tr_language_name(AppLanguage language)
{
    static const char *const names[] = {"English", "Espanol", "Francais", "Deutsch"};
    if ((unsigned int)language >= APP_LANGUAGE_COUNT)
        language = APP_LANGUAGE_ENGLISH;
    return names[language];
}

void tr_format_daily_summary(char *out, size_t out_size, AppLanguage language,
                             int weather_code, float high, float low)
{
    const char *condition = tr_condition(language, weather_code);
    switch (language) {
    case APP_LANGUAGE_SPANISH:
        snprintf(out, out_size, "%s. Max %.0f°, min %.0f°.",
                 condition, high, low);
        break;
    case APP_LANGUAGE_FRENCH:
        snprintf(out, out_size, "%s. Max %.0f°, min %.0f°.",
                 condition, high, low);
        break;
    case APP_LANGUAGE_GERMAN:
        snprintf(out, out_size, "%s. Hoch %.0f°, tief %.0f°.",
                 condition, high, low);
        break;
    default:
        snprintf(out, out_size, "%s. High %.0f°, low %.0f°.",
                 condition, high, low);
        break;
    }
}

void tr_format_precip_summary(char *out, size_t out_size, AppLanguage language,
                              int precipitation_percent)
{
    switch (language) {
    case APP_LANGUAGE_SPANISH:
        snprintf(out, out_size, precipitation_percent >= 60
                     ? "Precipitacion probable: %d%%."
                     : "Probabilidad de precipitacion: %d%%.",
                 precipitation_percent);
        break;
    case APP_LANGUAGE_FRENCH:
        snprintf(out, out_size, precipitation_percent >= 60
                     ? "Precipitations probables : %d%%."
                     : "Risque de precipitations : %d%%.",
                 precipitation_percent);
        break;
    case APP_LANGUAGE_GERMAN:
        snprintf(out, out_size, precipitation_percent >= 60
                     ? "Niederschlag wahrscheinlich: %d%%."
                     : "Niederschlagsrisiko: %d%%.",
                 precipitation_percent);
        break;
    default:
        snprintf(out, out_size, precipitation_percent >= 60
                     ? "Precipitation is likely: %d%%."
                     : "Chance of precipitation: %d%%.",
                 precipitation_percent);
        break;
    }
}

const char *tr_condition(AppLanguage language, int code)
{
    int condition = 13;
    if (code == 0) condition = 0;
    else if (code == 1) condition = 1;
    else if (code == 2) condition = 2;
    else if (code == 3) condition = 3;
    else if (code == 45 || code == 48) condition = 4;
    else if (code >= 51 && code <= 57) condition = 5;
    else if (code == 61) condition = 6;
    else if (code == 63 || code == 66 || code == 67) condition = 7;
    else if (code == 65) condition = 8;
    else if (code >= 71 && code <= 77) condition = 9;
    else if (code >= 80 && code <= 82) condition = 10;
    else if (code == 85 || code == 86) condition = 11;
    else if (code >= 95) condition = 12;
    static const char *const names[APP_LANGUAGE_COUNT][14] = {
        {"Clear sky", "Mainly clear", "Partly cloudy", "Overcast", "Foggy", "Drizzle",
         "Light rain", "Rain", "Heavy rain", "Snow", "Rain showers", "Snow showers",
         "Thunderstorms", "Weather"},
        {"Cielo despejado", "Mayormente despejado", "Parcialmente nublado", "Cubierto", "Niebla", "Llovizna",
         "Lluvia ligera", "Lluvia", "Lluvia intensa", "Nieve", "Chubascos", "Nevadas",
         "Tormentas", "Tiempo"},
        {"Ciel degage", "Plutot degage", "Partiellement nuageux", "Couvert", "Brouillard", "Bruine",
         "Pluie faible", "Pluie", "Forte pluie", "Neige", "Averses", "Averses de neige",
         "Orages", "Meteo"},
        {"Klar", "Uberwiegend klar", "Teilweise bewolkt", "Bedeckt", "Neblig", "Nieselregen",
         "Leichter Regen", "Regen", "Starkregen", "Schnee", "Regenschauer", "Schneeschauer",
         "Gewitter", "Wetter"}
    };
    if ((unsigned int)language >= APP_LANGUAGE_COUNT)
        language = APP_LANGUAGE_ENGLISH;
    return names[language][condition];
}

void tr_format_time(char *out, size_t out_size, const char *time_24,
                    int use_24_hour, AppLanguage language)
{
    (void)language;
    if (!time_24 || !time_24[0]) {
        snprintf(out, out_size, "--:--");
        return;
    }
    if (use_24_hour || !strchr(time_24, ':')) {
        snprintf(out, out_size, "%s", time_24);
        return;
    }
    int hour = 0;
    int minute = 0;
    if (sscanf(time_24, "%d:%d", &hour, &minute) != 2) {
        snprintf(out, out_size, "%s", time_24);
        return;
    }
    const char *suffix = hour >= 12 ? "PM" : "AM";
    int display_hour = hour % 12;
    if (!display_hour) display_hour = 12;
    snprintf(out, out_size, "%d:%02d %s", display_hour, minute, suffix);
}
