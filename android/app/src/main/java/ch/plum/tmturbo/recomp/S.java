package ch.plum.tmturbo.recomp;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;

import java.util.Locale;

/**
 * UI texts in the chosen language (res/values*: English, German, French, Spanish, Italian).
 * "auto" follows the system language; other languages fall back to English.
 */
final class S {
    /** Stored values of the "lang" setting; their labels are R.string.set_lang_vals. */
    static final String LANGS = "auto|en|de|fr|es|it";

    private static Resources res;
    private static Locale locale = Locale.getDefault();

    private S() { }

    static void apply(Context c, String lang) {
        Locale l = lang == null || lang.isEmpty() || lang.equals("auto")
                ? Resources.getSystem().getConfiguration().getLocales().get(0) : new Locale(lang);
        Configuration conf = new Configuration(c.getResources().getConfiguration());
        conf.setLocales(new LocaleList(l));
        res = c.createConfigurationContext(conf).getResources();
        locale = l;
    }

    static String t(int id) { return res.getString(id); }

    /** A string resource with format arguments, numbers formatted for the chosen language. */
    static String f(int id, Object... args) { return String.format(locale, res.getString(id), args); }

    /** The choices of a setting: a "|"-separated string resource. */
    static String[] list(int id) { return res.getString(id).split("\\|"); }

    /** Labels of the language setting: "automatic" translated, the languages in their own names. */
    static String[] langLabels() {
        return new String[] { t(R.string.lang_auto), "English", "Deutsch", "Français", "Español", "Italiano" };
    }
}
