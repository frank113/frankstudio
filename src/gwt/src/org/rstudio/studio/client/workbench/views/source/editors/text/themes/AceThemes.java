/*
 * AceThemes.java
 *
 * Copyright (C) 2009-18 by RStudio, Inc.
 *
 * This program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
package org.rstudio.studio.client.workbench.views.source.editors.text.themes;

import com.google.gwt.core.client.GWT;
import com.google.gwt.core.client.JsArray;
import com.google.gwt.core.client.JsArrayInteger;
import com.google.gwt.dom.client.Document;
import com.google.gwt.dom.client.Element;
import com.google.gwt.dom.client.LinkElement;
import com.google.gwt.dom.client.Style;
import com.google.gwt.user.client.Timer;
import com.google.inject.Inject;
import com.google.inject.Provider;
import com.google.inject.Singleton;

import org.rstudio.core.client.ColorUtil.RGBColor;
import org.rstudio.core.client.Debug;
import org.rstudio.core.client.dom.DomUtils;
import org.rstudio.core.client.widget.Operation;
import org.rstudio.core.client.widget.ProgressIndicator;
import org.rstudio.studio.client.application.Desktop;
import org.rstudio.studio.client.application.events.EventBus;
import org.rstudio.studio.client.common.DelayedProgressRequestCallback;
import org.rstudio.studio.client.server.ServerError;
import org.rstudio.studio.client.server.ServerRequestCallback;
import org.rstudio.studio.client.server.Void;
import org.rstudio.studio.client.workbench.prefs.model.UIPrefs;
import org.rstudio.studio.client.workbench.views.source.editors.text.events.EditorThemeChangedEvent;
import org.rstudio.studio.client.workbench.views.source.editors.text.themes.model.ThemeServerOperations;

import java.util.HashMap;
import java.util.function.Consumer;

@Singleton
public class AceThemes
{
   public static final String CHROME = "Chrome";
   public static final String CLOUDS = "Clouds";
   public static final String CLOUDS_MIDNIGHT = "Clouds Midnight";
   public static final String COBALT = "Cobalt";
   public static final String CRIMSON_EDITOR = "Crimson Editor";
   public static final String DAWN = "Dawn";
   public static final String DREAMWEAVER = "Dreamweaver";
   public static final String ECLIPSE = "Eclipse";
   public static final String IDLE_FINGERS = "Idle Fingers";
   public static final String KR_THEME = "KR Theme";
   public static final String MERBIVORE = "Merbivore";
   public static final String MERBIVORE_SOFT = "Merbivore Soft";
   public static final String MONO_INDUSTRIAL = "Mono Industrial";
   public static final String MONOKAI = "Monokai";
   public static final String NEON = "Neon";
   public static final String PASTEL_ON_DARK = "Pastel on Dark";
   public static final String SOLARIZED_DARK = "Solarized Dark";
   public static final String SOLARIZED_LIGHT = "Solarized Light";
   public static final String TEXTMATE = "TextMate";
   public static final String TOMORROW = "Tomorrow";
   public static final String TOMORROW_NIGHT = "Tomorrow Night";
   public static final String TOMORROW_NIGHT_80S = "Tomorrow Night 80's";
   public static final String TOMORROW_NIGHT_BLUE = "Tomorrow Night Blue";
   public static final String TOMORROW_NIGHT_BRIGHT = "Tomorrow Night Bright";
   public static final String TWILIGHT = "Twilight";
   public static final String VIBRANT_INK = "Vibrant Ink";

   @Inject
   public AceThemes(ThemeServerOperations themeServerOperations,
                    final Provider<UIPrefs> prefs,
                    EventBus events)
   {
      themeServerOperations_ = themeServerOperations;
      events_ = events;
      prefs_ = prefs;
      
      themes_ = new ArrayList<String>();
      themesByName_ = new HashMap<String, String>();

      addTheme(CHROME, res.chrome());
      addTheme(CLOUDS, res.clouds());
      addTheme(CLOUDS_MIDNIGHT, res.clouds_midnight());
      addTheme(COBALT, res.cobalt());
      addTheme(CRIMSON_EDITOR, res.crimson_editor());
      addTheme(DAWN, res.dawn());
      addTheme(DREAMWEAVER, res.dreamweaver());
      addTheme(ECLIPSE, res.eclipse());
      addTheme(IDLE_FINGERS, res.idle_fingers());
      addTheme(KR_THEME, res.kr_theme());
      addTheme(MERBIVORE, res.merbivore());
      addTheme(MERBIVORE_SOFT, res.merbivore_soft());
      addTheme(MONO_INDUSTRIAL, res.mono_industrial());
      addTheme(MONOKAI, res.monokai());
      addTheme(NEON, res.neon());
      addTheme(PASTEL_ON_DARK, res.pastel_on_dark());
      addTheme(SOLARIZED_DARK, res.solarized_dark());
      addTheme(SOLARIZED_LIGHT, res.solarized_light());
      addTheme(TEXTMATE, res.textmate());
      addTheme(TOMORROW, res.tomorrow());
      addTheme(TOMORROW_NIGHT, res.tomorrow_night());
      addTheme(TOMORROW_NIGHT_80S, res.tomorrow_night_eighties());
      addTheme(TOMORROW_NIGHT_BLUE, res.tomorrow_night_blue());
      addTheme(TOMORROW_NIGHT_BRIGHT, res.tomorrow_night_bright());
      addTheme(TWILIGHT, res.twilight());
      addTheme(VIBRANT_INK, res.vibrant_ink());

      prefs.get().theme().bind(theme -> applyTheme(theme));
   }
   
   private void applyTheme(Document document, final AceTheme theme)
   {
      // Build a relative path to avoid having to register 80000 theme URI handlers on the server.
      int pathUpCount = 0;
      String baseUrl = GWT.getHostPageBaseURL();
      
      // Strip any query out of the current URL since it isn't relevant to the path.
      String currentUrl = document.getURL().split("\\?")[0];
      if (!currentUrl.equals(baseUrl) &&
         currentUrl.indexOf(baseUrl) == 0)
      {
         pathUpCount = currentUrl.substring(baseUrl.length()).split("/").length;
      }
      
      // Build the URL.
      StringBuilder urlBuilder = new StringBuilder();
      for (int i = 0; i < pathUpCount; ++i)
      {
         urlBuilder.append("../");
      }
      urlBuilder.append(theme.getUrl())
         .append("?dark=")
         .append(theme.isDark() ? "1" : "0");
      
      LinkElement currentStyleEl = document.createLinkElement();
      currentStyleEl.setType("text/css");
      currentStyleEl.setRel("stylesheet");
      currentStyleEl.setId(linkId_);
      currentStyleEl.setHref(urlBuilder.toString());
   
      Element oldStyleEl = document.getElementById(linkId_);
      if (null != oldStyleEl)
      {
        document.getBody().replaceChild(currentStyleEl, oldStyleEl);
      }
      else
      {
         document.getBody().appendChild(currentStyleEl);
      }
      
      if(theme.isDark())
         document.getBody().addClassName("editor_dark");
      else
         document.getBody().removeClassName("editor_dark");
         
      
      // Deferred so that the browser can render the styles.
      new Timer()
      {
         @Override
         public void run()
         {
            events_.fireEvent(new EditorThemeChangedEvent(theme));
            
            // synchronize the effective background color with the desktop
            if (Desktop.isDesktop())
            {
               // find 'rstudio_container' element (note that this may not exist
               // in some satellite windows; e.g. the Git window)
               Element el = Document.get().getElementById("rstudio_container");
               if (el == null)
                  return;
               
               Style style = DomUtils.getComputedStyles(el);
               String color = style.getBackgroundColor();
               RGBColor parsed = RGBColor.fromCss(color);
               
               JsArrayInteger colors = JsArrayInteger.createArray(3).cast();
               colors.set(0, parsed.red());
               colors.set(1, parsed.green());
               colors.set(2, parsed.blue());
               Desktop.getFrame().setBackgroundColor(colors);
               Desktop.getFrame().syncToEditorTheme(theme.isDark());

               el = DomUtils.getElementsByClassName("rstheme_toolbarWrapper")[0];
               style = DomUtils.getComputedStyles(el);
               color = style.getBackgroundColor();
               parsed = RGBColor.fromCss(color);

               Desktop.getFrame().changeTitleBarColor(parsed.red(), parsed.green(), parsed.blue());
            }
         }
      }.schedule(100);
   }

   private void applyTheme(final AceTheme theme)
   {
      applyTheme(Document.get(), theme);
   }

   public void applyTheme(Document document)
   {
      applyTheme(document, prefs_.get().theme().getValue());
   }
   
   public void getThemes(
      Consumer<HashMap<String, AceTheme>> themeConsumer,
      ProgressIndicator indicator)
   {
      themeServerOperations_.getThemes(
         new DelayedProgressRequestCallback<JsArray<AceTheme>>(indicator)
      {
         @Override
         public void onSuccess(JsArray<AceTheme> jsonThemeArray)
         {
            themes_.clear();
            int len = jsonThemeArray.length();
            for (int i = 0; i < len; ++i)
            {
               AceTheme theme = jsonThemeArray.get(i);
               themes_.put(theme.getName(), theme);
            }
            
            if (len == 0)
               Debug.logWarning("Server was unable to find any installed themes.");
            themeConsumer.accept(themes_);
         }
      });
   }
   
   public void addTheme(
      String themeLocation,
      Consumer<String> stringConsumer,
      Consumer<String> errorMessageConsumer)
   {
      themeServerOperations_.addTheme(new ServerRequestCallback<String>()
      {
         @Override
         public void onResponseReceived(String result)
         {
            stringConsumer.accept(result);
         }
         
         @Override
         public void onError(ServerError error)
         {
            errorMessageConsumer.accept(error.getUserMessage());
         }
      }, themeLocation);
   }
   
   public void removeTheme(
      String themeName,
      Consumer<String> errorMessageConsumer,
      Operation afterOperation)
   {
      if (!themes_.containsKey(themeName))
      {
         errorMessageConsumer.accept("The specified theme does not exist");
      }
      else if (themes_.get(themeName).isDefaultTheme())
      {
         errorMessageConsumer.accept("The specified theme is a default RStudio theme and cannot be removed.");
      }
      else
      {
         themeServerOperations_.removeTheme(
            new ServerRequestCallback<Void>()
            {
               @Override
               public void onResponseReceived(Void response)
               {
                  themes_.remove(themeName);
                  afterOperation.execute();
               }
               
               @Override
               public void onError(ServerError error)
               {
                  errorMessageConsumer.accept(error.getUserMessage());
               }
            },
            themeName);
      }
   }
   
   // This function can be used to get the name of a theme without adding it to RStudio. It is not
   // used to get the name of an existing theme.
   public void getThemeName(
      String themeLocation,
      Consumer<String> stringConsumer,
      Consumer<String> errorMessageConsumer)
   {
      themeServerOperations_.getThemeName(
         new ServerRequestCallback<String>()
         {
            @Override
            public void onResponseReceived(String result)
            {
               stringConsumer.accept(result);
            }
            
            @Override
            public void onError(ServerError error)
            {
               errorMessageConsumer.accept(error.getUserMessage());
            }
         },
         themeLocation);
   }

   private ThemeServerOperations themeServerOperations_;
   private final EventBus events_;
   private final Provider<UIPrefs> prefs_;
   private final String linkId_ = "rstudio-acethemes-linkelement";
   private HashMap<String, AceTheme> themes_;
}
