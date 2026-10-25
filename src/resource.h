// scripts in the extras directory will search this for strings as to which keys to include in KeyMap
// if a define is to be included in the publicly-exposed definitions, start the comment with ":KeyMap:"
// don't worry about the formatting, the script that auto-generates the definition will take care of it

// title for main window and msgbox so it can be change via actions
#define APP_TITLE "mkImageViewer"

// this is not used anywhere in code, but defined in KeyMap as an "invalid command", so make sure the defined number doesn't match anything!
#define IDM_DONOTHING        1            // :KeyMap: do nothing (can be used to disable a standard command for a mouse button)

#define IDM_DEFAULT_ESC        255
#define IDM_STOP_MOVIE        1000
#define IDM_MINIMIZE        1010        // :KeyMap: minimize window
#define IDM_EXPLORE            1101        // :KeyMap: open and select file in Explorer
#define IDM_SETTINGS_DIALOG    1150      // :KeyMap: show settings dialog
#define IDM_RELOAD            1600        // :KeyMap: reload image
#define IDM_RELOAD_FULL        1610        // :KeyMap: reload image and file list
#define IDM_COPY            2000        // :KeyMap: copy image to clipboard in screen size
#define IDM_COPY_FULL        2500        // :KeyMap: copy image to clipboard in original size
#define IDM_COPY_PATH        2525        // :KeyMap: copy image path to clipboard
#define IDM_CUT_FILE        2530        // :KeyMap: cut image file to clipboard (for paste action in file manager)
#define IDM_DELETE_FILE        2623        // :KeyMap: delete file completely
#define IDM_NEXT            4000        // :KeyMap: go to next image
#define IDM_PREV            5000        // :KeyMap: go to previous image
#define IDM_FIRST            5100        // :KeyMap: go to first image
#define IDM_LAST            5200        // :KeyMap: go to last image
#define IDM_NEXTFILE        5300        // :KeyMap: go to next image file (leave multiframe container)
#define IDM_PREVFILE        5301        // :KeyMap: go to previous image file (leave multiframe container)
#define IDM_PLAYPAUSENEXT    5302        // :KeyMap: if current image is animation: play/pause, otherwise go to next image
#define IDM_PLAYPAUSE        5303        // :KeyMap: if current image is animation: play/pause
#define IDM_NEXTFRAME       5304        // :KeyMap: go to next frame (in multiframe container)
#define IDM_LOOP_FOLDER        6000        // :KeyMap: set navigation mode loop through folder
#define IDM_LOOP_RECURSIVELY 6010        // :KeyMap: set navigation mode loop through folder and subfolders
#define IDM_LOOP_SIBLINGS    6020        // :KeyMap: set navigation mode loop through folders on same level
#define IDM_SORT_MOD_DATE    7000        // :KeyMap: sorting order by modification date
#define IDM_SORT_NAME        7020        // :KeyMap: sorting order by name
#define IDM_SORT_RANDOM        7030        // :KeyMap: sorting order random
#define IDM_SORT_SIZE        7040        // :KeyMap: sorting order file size in bytes
#define IDM_SORT_ASCENDING    7100        // :KeyMap: sort ascending (increasing in value, e.g. A->Z, 0->9)
#define IDM_SORT_DESCENDING 7110        // :KeyMap: sort descending (decreasing in value, e.g. Z->A, 9->0)
#define IDM_SLIDESHOW_RESUME 7399        // :KeyMap: resume slide show (after stop)
#define IDM_SLIDESHOW_START 7400
#define IDM_SLIDESHOW_1        7401        // :KeyMap: start slide show with 1 second timer
#define IDM_SLIDESHOW_2        7402        // :KeyMap: start slide show with 2 second timer
#define IDM_SLIDESHOW_3        7403        // :KeyMap: start slide show with 3 second timer
#define IDM_SLIDESHOW_4        7404        // :KeyMap: start slide show with 4 second timer
#define IDM_SLIDESHOW_5        7405        // :KeyMap: start slide show with 5 second timer
#define IDM_SLIDESHOW_6        7406        // :KeyMap: start slide show with 6 second timer
#define IDM_SLIDESHOW_7        7407        // :KeyMap: start slide show with 7 second timer
#define IDM_SLIDESHOW_8        7408        // :KeyMap: start slide show with 8 second timer
#define IDM_SLIDESHOW_9        7409        // :KeyMap: start slide show with 9 second timer
#define IDM_SLIDESHOW_10    7410        // :KeyMap: start slide show with 10 second timer
#define IDM_SLIDESHOW_20    7420        // :KeyMap: start slide show with 20 second timer
#define IDM_EFFECT_NONE     7450
#define IDM_EFFECT_BLEND    7451
#define IDM_EFFECT_SLIDE_RL 7452
#define IDM_EFFECT_SLIDE_LR 7453
#define IDM_EFFECT_SLIDE_TB 7454
#define IDM_EFFECT_SLIDE_BT 7455
#define IDM_EFFECT_ROLL_RL  7456
#define IDM_EFFECT_ROLL_LR  7457
#define IDM_EFFECT_ROLL_TB  7458
#define IDM_EFFECT_ROLL_BT  7459
#define IDM_EFFECT_SCROLL_RL 7460
#define IDM_EFFECT_SCROLL_LR 7461
#define IDM_EFFECT_SCROLL_TB 7462
#define IDM_EFFECT_SCROLL_BT 7463
#define IDM_EFFECTTIME_VERY_FAST  7470
#define IDM_EFFECTTIME_FAST  7471
#define IDM_EFFECTTIME_NORMAL 7472
#define IDM_EFFECTTIME_SLOW  7473
#define IDM_EFFECTTIME_VERY_SLOW  7474
#define IDM_MOVIE_START_FPS 7500 // Pseudo entry (diff of the rest of the IDM_MOVIE_* values used to calculate actual FPS)
#define IDM_MOVIE_5_FPS        7505        // :KeyMap: Start animating at 5 fps
#define IDM_MOVIE_10_FPS    7510        // :KeyMap: Start animating at 10 fps
#define IDM_MOVIE_15_FPS    7515        // :KeyMap: Start animating at 15 fps
#define IDM_MOVIE_20_FPS    7520        // :KeyMap: Start animating at 20 fps
#define IDM_MOVIE_25_FPS    7525        // :KeyMap: Start animating at 25 fps
#define IDM_MOVIE_30_FPS    7530        // :KeyMap: Start animating at 30 fps
#define IDM_MOVIE_50_FPS    7550        // :KeyMap: Start animating at 50 fps
#define IDM_MOVIE_60_FPS    7560        // :KeyMap: Start animating at 60 fps
#define IDM_MOVIE_100_FPS    7600        // :KeyMap: Start animating at 100 fps
#define IDM_ROTATE_90        8000        // :KeyMap: rotate image 90 deg
#define IDM_ROTATE_270        9000        // :KeyMap: rotate image 270 deg
#define IDM_MIRROR_H        9200        // :KeyMap: mirror image horizontally
#define IDM_MIRROR_V        9300        // :KeyMap: mirror image vertically
#define IDM_AUTO_CORRECTION    10000        // :KeyMap: toggle auto color and contrast correction
#define IDM_KEEP_PARAMETERS    11000        // :KeyMap: toogle keep parameters between images
#define IDM_FIT_TO_SCREEN    12000        // :KeyMap: fit image to screen
#define IDM_FILL_WITH_CROP    12001        // :KeyMap: fit image to fill screen with crop
#define IDM_FIT_TO_SCREEN_NO_ENLARGE 12002
#define IDM_FULL_SCREEN_MODE 12011        // :KeyMap: toggle full screen mode
#define IDM_ZOOM_400        12020        // :KeyMap: zoom to 400 %
#define IDM_ZOOM_200        12030        // :KeyMap: zoom to 200 %
#define IDM_ZOOM_100        12040        // :KeyMap: zoom to 100 %
#define IDM_ZOOM_50            12050        // :KeyMap: zoom to 50 %
#define IDM_ZOOM_25            12060        // :KeyMap: zoom to 25 %
#define IDM_ZOOM_INC        12100        // :KeyMap: zoom in
#define IDM_ZOOM_DEC        12200        // :KeyMap: zoom out
#define IDM_ZOOM_MODE        12300

#define IDM_TOGGLE_SORT_RANDOM    12450        // :KeyMap: toggle between random and name sort
#define IDM_TOGGLE_INFO_OVERLAY    12452        // :KeyMap: toggle custom overlay
#define IDM_CHANGE_FOLDER_LANGUAGE    12453    // :KeyMap: jump between different language folders (varying in endings like " [en]", " [de]", " [ko]")
#define IDM_OPEN_DEFAULT_EDITOR        12454    // :KeyMap: open image with default registry edit action of this file type
#define IDM_TOGGLE_AUTO_ZOOM_FIT    12455    // :KeyMap: toggle auto zoom mode "BookMode (Default) <-> Fit " and "Fit (Default) <-> None"
#define IDM_TOGGLE_USE_EMBEDDED_COLOR_PROFILES    12456    // :KeyMap: toggle INI setting "UseEmbeddedColorProfiles"

#define IDM_AUTO_ZOOM_NONE    12500        // :KeyMap: set auto zoom mode disabled
#define IDM_AUTO_ZOOM_FIT_NO_ZOOM    12510        // :KeyMap: set auto zoom mode fit to screen, never zoom
#define IDM_AUTO_ZOOM_FILL_NO_ZOOM    12520        // :KeyMap: set auto zoom mode fill screen, never zoom
#define IDM_AUTO_ZOOM_FIT    12530        // :KeyMap: set auto zoom mode fit to screen
#define IDM_AUTO_ZOOM_FILL    12540        // :KeyMap: set auto zoom mode fill screen
#define IDM_AUTO_ZOOM_BOOK_MODE    12550        // :KeyMap: set auto zoom mode to book mode
#define IDM_EDIT_USER_CONFIG   12610        // :KeyMap: edit user configuration
#define IDM_EXIT            13000        // :KeyMap: exit application
#define IDM_HELP            14000
#define IDM_COLOR_CORRECTION_INC    14200        // :KeyMap: increase color correction amount
#define IDM_COLOR_CORRECTION_DEC    14201        // :KeyMap: decrease color correction amount
#define IDM_TOGGLE_TRANSPARENCY        14202        // :KeyMap: toggle transparency mode
#define IDM_CONTRAST_CORRECTION_INC    14300        // :KeyMap: increase contrast correction amount
#define IDM_CONTRAST_CORRECTION_DEC    14301        // :KeyMap: decrease contrast correction amount
#define IDM_TOGGLE_RESAMPLING_QUALITY    14800        // :KeyMap: toggle resampling quality (between point sampling and filtering)
#define IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS    15000        // :KeyMap: toggle between fit to screen and 100 % zoom
#define IDM_TOGGLE_FILL_WITH_CROP_100_PERCENTS    15001        // :KeyMap: toggle between fill with crop and 100 % zoom
#define IDM_EXCHANGE_PROC_PARAMS    15100        // :KeyMap: toggle between two sets of image processing parameters
#define IDM_PAN_UP    15200        // :KeyMap: pan up
#define IDM_PAN_DOWN    15201        // :KeyMap: pan down
#define IDM_PAN_RIGHT    15202        // :KeyMap: pan right
#define IDM_PAN_LEFT    15203        // :KeyMap: pan left
#define IDM_CONTEXT_MENU    16000        // :KeyMap: display context menu
