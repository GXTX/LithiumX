// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022 Ryzee119

#include "lithiumx.h"
#include <src/misc/lv_lru.h>
#include <src/misc/lv_ll.h>

parse_handle_t *parsers[DASH_MAX_PAGES];
static lv_obj_t *page_tiles;
static lv_obj_t *label_footer;
static int page_current;
static lv_lru_t *thumbnail_cache;
static size_t thumbnail_cache_size = (16 * 1024 * 1024);

typedef struct
{
    lv_obj_t *image_container;
} jpeg_ll_value_t;
static lv_ll_t jpeg_decomp_list;

void dash_scroller_set_page()
{
    toml_array_t *pages = toml_array_in(dash_search_paths, "pages");
    int page_max = LV_MIN(toml_array_nelem(pages), DASH_MAX_PAGES);
    page_current = LV_CLAMP(0, page_current, page_max - 1);
    lv_obj_set_tile_id(page_tiles, page_current, 0, LV_ANIM_ON);
    lv_obj_t *tile = lv_tileview_get_tile_act(page_tiles);
    lv_obj_t *scroller = lv_obj_get_child(tile, 0);
    int *current_index = (int *)&scroller->user_data;
    *current_index = LV_MAX(1, *current_index);

    assert(lv_obj_get_child_cnt(scroller) >= 1);

    lv_obj_t *focus_item = lv_obj_get_child(scroller, *current_index);
    if (lv_obj_is_valid(focus_item) == false)
    {
        // We were are trying to focus an invalid object, revert to index 0 which should always
        // be present
        *current_index = 0;
        focus_item = lv_obj_get_child(scroller, *current_index);
        assert(lv_obj_is_valid(scroller));
    }

    dash_focus_set_final(lv_obj_get_child(scroller, 0));
    dash_focus_change(focus_item);
}

static void jpg_decompression_complete_cb(void *img, void *mem, int w, int h, void *user_data)
{
    lv_obj_t *image_container = user_data;
    title_t *t = image_container->user_data;

    if (img == NULL)
    {
        t->jpg_info->decomp_handle = NULL;
        return;
    }

    lvgl_getlock();

    t->jpg_info->mem = mem;
    t->jpg_info->image = img;
    t->jpg_info->w = w;
    t->jpg_info->h = h;
    t->jpg_info->decomp_handle = NULL;

    t->jpg_info->canvas = lv_canvas_create(image_container);
    lv_canvas_set_buffer(t->jpg_info->canvas, img, w, h, LV_IMG_CF_TRUE_COLOR);

    lv_img_set_size_mode(t->jpg_info->canvas, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_zoom(t->jpg_info->canvas, DASH_THUMBNAIL_WIDTH * 256 / w);
    lv_obj_mark_layout_as_dirty(t->jpg_info->canvas);

    lv_lru_set(thumbnail_cache, t, sizeof(title_t *), t, w * h * ((LV_COLOR_DEPTH + 7) / 8));
    lvgl_removelock();
}

static void update_thumbnail_callback(lv_event_t *event)
{
    lv_obj_t *image_container = lv_event_get_target(event);
    title_t *t = image_container->user_data;

    if (t->jpg_info == NULL)
    {
        return;
    }

    if (t->jpg_info->decomp_handle == NULL && t->jpg_info->mem == NULL)
    {
        t->jpg_info->decomp_handle = jpeg_decoder_queue(t->jpg_info->thumb_path,
                                                        jpg_decompression_complete_cb, image_container);
        jpeg_ll_value_t *n = _lv_ll_ins_tail(&jpeg_decomp_list);
        n->image_container = image_container;
    }
}

static int get_launch_path_callback(void *param, int argc, char **argv, char **azColName)
{
    (void) param;
    (void) azColName;
    (void) argc;
    assert(argc == 1);
    
    lvgl_getlock();
    char *launch_path = lv_mem_alloc(DASH_MAX_PATH);
    strncpy(launch_path, argv[0], DASH_MAX_PATH);
    dash_launch_path = launch_path;
    lvgl_removelock();
    return 0;
}

void launch_title_callback(void *param)
{
    char cmd[SQL_MAX_COMMAND_LEN];
    char time_str[20];

    title_t *t = param;
    lv_snprintf(cmd, sizeof(cmd), SQL_TITLE_GET_LAUNCH_PATH, t->db_id);
    db_command_with_callback(cmd, get_launch_path_callback, NULL);

    platform_get_iso8601_time(time_str);
    lv_snprintf(cmd, sizeof(cmd), SQL_TITLE_SET_LAST_LAUNCH_DATETIME, time_str, t->db_id);
    db_command_with_callback(cmd, NULL, NULL);

    lv_set_quit(LV_QUIT_OTHER);
}

static void item_selection_callback(lv_event_t *event)
{
    lv_event_code_t e = lv_event_get_code(event);

    lv_obj_t *item_container = lv_event_get_target(event);
    title_t *t = item_container->user_data;
    if (e == LV_EVENT_FOCUSED || e == LV_EVENT_DEFOCUSED)
    {
        lv_style_value_t border_width;
        lv_style_value_t border_colour;
        lv_style_get_prop(&titleview_image_container_style, LV_STYLE_BORDER_WIDTH, &border_width);
        lv_style_get_prop(&titleview_image_container_style, LV_STYLE_BORDER_COLOR, &border_colour);
        if (e == LV_EVENT_FOCUSED)
        {
            lv_obj_set_style_border_width(item_container, border_width.num * 2, LV_PART_MAIN);
            lv_obj_set_style_border_color(item_container, lv_color_white(), LV_PART_MAIN);
            lv_label_set_text(label_footer, t->title);
        }
        else
        {
            lv_obj_set_style_border_width(item_container, border_width.num, LV_PART_MAIN);
            lv_obj_set_style_border_color(item_container, border_colour.color, LV_PART_MAIN);
        }
    }
    else if (e == LV_EVENT_KEY)
    {
        lv_obj_t *scroller = lv_obj_get_parent(item_container);
        int *current_index = (int *)&scroller->user_data;

        lv_key_t key = *((lv_key_t *)lv_event_get_param(event));
        if (key == DASH_PREV_PAGE || key == DASH_NEXT_PAGE)
        {
            page_current += (key == DASH_NEXT_PAGE) ? (1) : -1;
            dash_scroller_set_page();
        }
        // L and R are the back triggers
        else if (key == LV_KEY_RIGHT || key == LV_KEY_LEFT || key == LV_KEY_UP || key == LV_KEY_DOWN || key == 'L' || key == 'R')
        {
            int last_index = lv_obj_get_child_cnt(scroller) - 1;
            int new_index = *current_index;

            // FIXME: Should really allow thumbnails of any width
            int tiles_per_row = lv_obj_get_width(scroller) / DASH_THUMBNAIL_WIDTH;

            // At the start, loop to end
            if (*current_index <= 1 && key == LV_KEY_UP)
            {
                new_index = last_index;
            }
            // At the end, loop to start
            else if (*current_index == last_index && key == LV_KEY_DOWN)
            {
                new_index = 1;
            }
            // Increment left or right one
            else if (key == LV_KEY_RIGHT || key == LV_KEY_LEFT)
            {
                new_index += (key == LV_KEY_RIGHT) ? 1 : -1;
            }
            // Increment up or down one
            else if (key == LV_KEY_UP || key == LV_KEY_DOWN)
            {
                if (new_index == 0)
                {
                    new_index = 1;
                }
                else
                {
                    new_index += (key == LV_KEY_DOWN) ? tiles_per_row : -tiles_per_row;
                }
            }
            // Increment up or down lots (LT and RT)
            else if (key == 'L' || key == 'R')
            {
                new_index += (key == 'R') ? (tiles_per_row * 8) : -(tiles_per_row * 8);
            }
            // Clamp the new index within the limits. Prefer index 1 as index 0 is the null
            // item. But will revert to 0 if no items in page
            new_index = LV_MAX(1, new_index);
            new_index = LV_CLAMP(0, new_index, last_index);
            *current_index = new_index;

            lv_obj_t *new_item_container = lv_obj_get_child(scroller, new_index);
            assert(lv_obj_is_valid(new_item_container));

            // Scroll until our new selection is in view
            lv_obj_scroll_to_view_recursive(new_item_container, LV_ANIM_ON);
            dash_focus_change(new_item_container);
        }
        else if (key == DASH_INFO_PAGE && *current_index != 0)
        {
            dash_synop_open((t->db_id));
        }
        else if (key == DASH_SETTINGS_PAGE)
        {
            dash_mainmenu_open();
        }
        else if (key == LV_KEY_ENTER && *current_index > 0)
        {
            confirmbox_open(t->title, launch_title_callback, t);
        }
    }
}

static void item_deletion_callback(lv_event_t *event)
{
    lv_obj_t *item_container = lv_event_get_target(event);
    title_t *t = item_container->user_data;
    if (t->jpg_info)
    {
        lv_lru_remove(thumbnail_cache, t, sizeof(title_t *));
        t->jpg_info->decomp_handle = NULL;
        lv_mem_free(t->jpg_info->thumb_path);
        lv_mem_free(t->jpg_info);
        lv_mem_free(t);
    }
}

// Callback when a new row is read from the SQL database. This is a new item to add
static int item_scan_callback(void *param, int argc, char **argv, char **azColName)
{
    parse_handle_t *p = param;
    lv_obj_t *scroller = p->scroller;
    (void) argc;
    (void) azColName;

    assert(strcmp(azColName[DB_INDEX_ID], SQL_TITLE_DB_ID) == 0);
    assert(strcmp(azColName[DB_INDEX_TITLE], SQL_TITLE_NAME) == 0);
    assert(strcmp(azColName[DB_INDEX_LAUNCH_PATH], SQL_TITLE_LAUNCH_PATH) == 0);

    lvgl_getlock();
    title_t *t = lv_mem_alloc(sizeof(title_t));
    assert(t);
    lv_memset(t, 0, sizeof(title_t));
    t->db_id = atoi(argv[DB_INDEX_ID]);

    lv_obj_t *item_container = lv_obj_create(scroller);
    item_container->user_data = t;
    lv_obj_add_style(item_container, &titleview_image_container_style, LV_PART_MAIN);
    lv_obj_clear_flag(item_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(item_container, DASH_THUMBNAIL_HEIGHT);
    lv_obj_set_width(item_container, DASH_THUMBNAIL_WIDTH);

    // Create a label with the game title
    lv_obj_t *label = lv_label_create(item_container);
    lv_obj_add_style(label, &titleview_image_text_style, LV_PART_MAIN);
    lv_obj_set_width(label, DASH_THUMBNAIL_WIDTH);
    lv_obj_update_layout(label);
    lv_label_set_text(label, argv[DB_INDEX_TITLE]);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    lv_group_add_obj(lv_group_get_default(), item_container);
    lv_obj_add_event_cb(item_container, item_selection_callback, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(item_container, item_selection_callback, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(item_container, item_selection_callback, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(item_container, item_deletion_callback, LV_EVENT_DELETE, NULL);

    strncpy(t->title, argv[DB_INDEX_TITLE], sizeof(t->title) - 1);

    // Check if a thumbnail exists
    char *thumb_path = lv_mem_alloc(DASH_MAX_PATH);
    assert(thumb_path);
    strncpy(thumb_path, argv[DB_INDEX_LAUNCH_PATH], 256);
    size_t len = strlen(thumb_path);
    assert(len > 3);
    strcpy(&thumb_path[len - 3], "tbn");
    DWORD fileAttributes = GetFileAttributes(thumb_path);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES || (fileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        lv_mem_free(thumb_path);
        t->jpg_info = NULL;
        lvgl_removelock();
        return 0;
    }

    // It has a thumbnail. Create a struct to store the info.
    t->jpg_info = lv_mem_alloc(sizeof(jpg_info_t));
    assert(t->jpg_info);
    lv_memset(t->jpg_info, 0, sizeof(jpg_info_t));
    t->jpg_info->thumb_path = thumb_path;
    lv_obj_add_event_cb(item_container, update_thumbnail_callback, LV_EVENT_DRAW_MAIN_END, NULL);

    lvgl_removelock();
    return 0;
}

static void dash_scroller_get_sort_strings(unsigned int sort_index, const char **sort_by, const char **order_by)
{
    switch (sort_index)
    {
    case DASH_SORT_RATING:
        *sort_by = SQL_TITLE_RATING;
        *order_by = "DESC";
        break;
    case DASH_SORT_LAST_LAUNCH:
        *sort_by = SQL_TITLE_LAST_LAUNCH;
        *order_by = "DESC";
        break;
    case DASH_SORT_RELEASE_DATE:
        *sort_by = SQL_TITLE_RELEASE_DATE;
        *order_by = "DESC";
        break;
    default:
        *sort_by = SQL_TITLE_NAME;
        *order_by = "ASC";
    }
}

static int db_scan_thread_f(void *param)
{
    parse_handle_t *p = param;
    char cmd[SQL_MAX_COMMAND_LEN];

    if (strcmp(p->page_title, "Recent") == 0)
    {
        lv_snprintf(cmd, sizeof(cmd), SQL_TITLE_GET_RECENT, settings_earliest_recent_date, settings_max_recent);
    }
    else
    {
        int sort_index = 0;
        const char *sort_by;
        const char *order_by;
        dash_scroller_get_sort_value(p->page_title, &sort_index);
        dash_scroller_get_sort_strings(sort_index, &sort_by, &order_by);
        lv_snprintf(cmd, sizeof(cmd), SQL_TITLE_GET_SORTED_LIST, "*", p->page_title, sort_by, order_by);
    }
    db_command_with_callback(cmd, item_scan_callback, param);
    return 0;
}

static void cache_free(title_t *t)
{
    assert(lv_obj_is_valid(t->jpg_info->canvas));
    jpeg_decoder_abort(t->jpg_info->decomp_handle);
    assert(t->jpg_info->mem);
    free(t->jpg_info->mem);
    lv_obj_del(t->jpg_info->canvas);
    t->jpg_info->decomp_handle = NULL;
    t->jpg_info->mem = NULL;
    t->jpg_info->image = NULL;
}

void dash_scroller_clear_page(const char *page_title)
{
    lv_obj_t *scroller = NULL;
    for (int i = 0; i < DASH_MAX_PAGES; i++)
    {
        if (parsers[i] == NULL)
        {
            continue;
        }
        if (strcmp(page_title, parsers[i]->page_title) == 0)
        {
            scroller = parsers[i]->scroller;
            lv_obj_t *item_container = lv_obj_get_child(scroller, 1);
            while (item_container)
            {
                lv_obj_del(item_container);
                item_container = lv_obj_get_child(scroller, 1);
            }
        }
    }
}

static void jpeg_clear_timer(lv_timer_t *t)
{
    (void) t;
    jpeg_ll_value_t *item = _lv_ll_get_head(&jpeg_decomp_list);
    while (item)
    {
        lv_obj_t *image_container = item->image_container;
        title_t *title = image_container->user_data;
        assert(title->jpg_info);
        // Still decompressing but no longer visible. Lets abort it.
        if (lv_obj_is_visible(image_container) == false)
        {
            jpeg_decoder_abort(title->jpg_info->decomp_handle);
            title->jpg_info->decomp_handle = NULL;
        }
        // Jpeg finished decomp (or was aborted already), dont need to it anymore
        if (title->jpg_info->decomp_handle == NULL)
        {
            _lv_ll_remove(&jpeg_decomp_list, item);
            lv_mem_free(item);
            item = _lv_ll_get_head(&jpeg_decomp_list);
        }
        // Still compressing, but still visible. Try next item
        else
        {
            item = _lv_ll_get_next(&jpeg_decomp_list, item);
        }
    }
}

void dash_scroller_init()
{
    lv_coord_t w = lv_obj_get_width(lv_scr_act());
    lv_coord_t h = lv_obj_get_height(lv_scr_act());
    page_current = settings_default_page_index;

    lv_memset(parsers, 0, sizeof(parsers));
    thumbnail_cache = lv_lru_create(thumbnail_cache_size, 175 * 248 * (LV_COLOR_DEPTH + 7) / 8,
                                    (lv_lru_free_t *)cache_free, NULL);

    jpeg_decoder_init(LV_COLOR_DEPTH, 256);

    _lv_ll_init(&jpeg_decomp_list, sizeof(jpeg_ll_value_t));
    lv_timer_create(jpeg_clear_timer, LV_DISP_DEF_REFR_PERIOD, NULL);
 
    // Create a tileview object to manage different pages
    page_tiles = lv_tileview_create(lv_scr_act());
    lv_obj_align(page_tiles, LV_ALIGN_TOP_MID, 0, DASH_YMARGIN);
    lv_obj_set_size(page_tiles, w - (2 * DASH_XMARGIN), h - (2 * DASH_YMARGIN));
    lv_obj_set_style_bg_opa(page_tiles, 0, LV_PART_MAIN);
    lv_obj_clear_flag(page_tiles, LV_OBJ_FLAG_SCROLLABLE);

    // Create a footer label which shows the current item name
    label_footer = lv_label_create(lv_scr_act());
    lv_obj_align(label_footer, LV_ALIGN_BOTTOM_MID, 0, -DASH_YMARGIN);
    lv_obj_add_style(label_footer, &titleview_header_footer_style, LV_PART_MAIN);
    lv_label_set_text(label_footer, "");
    lv_obj_update_layout(label_footer);
}

void dash_scroller_scan_db()
{
    toml_table_t *paths = dash_search_paths;
    toml_array_t *pages = toml_array_in(paths, "pages");
    int dash_num_pages = LV_MIN(toml_array_nelem(pages), DASH_MAX_PAGES);

    lv_obj_clean(page_tiles);
    for (int i = 0; i < DASH_MAX_PAGES; i++)
    {
        parse_handle_t *parser = parsers[i];
        if (parser == NULL)
            continue;
        lv_mem_free(parser);
        parsers[i] = NULL;
    }

    // Create a parser object for each page. This includes a container to show our game art
    // and all the parse directories to find our items
    for (int i = 0; i < dash_num_pages; i++)
    {
        parse_handle_t *parser = lv_mem_alloc(sizeof(parse_handle_t));
        parsers[i] = parser;
        lv_obj_t **tile = &parser->tile;
        lv_obj_t **scroller = &parser->scroller;
        lv_memset(parser, 0, sizeof(parse_handle_t));

        // Create a new page in our tileview
        *tile = lv_tileview_add_tile(page_tiles, i, 0, LV_DIR_NONE);

        // Create a container that will have our scroller game art
        *scroller = lv_obj_create(*tile);
        (*scroller)->user_data = (void *)1; // Store the currently active index here

        // Create a header label for the page from the xml
        lv_obj_t *label_page_title = lv_label_create(*tile);
        toml_datum_t name_str = toml_string_in(toml_table_at(pages, i), "name");
        if (name_str.ok)
        {
            strncpy(parser->page_title, name_str.u.s, sizeof(parser->page_title) - 1);
        }
        lv_label_set_text(label_page_title, parser->page_title);
        lv_obj_align(label_page_title, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_style(label_page_title, &titleview_header_footer_style, LV_PART_MAIN);
        lv_obj_update_layout(label_page_title);

        // Setup the container for our scrolling game art
        int sc_parent_w = lv_obj_get_width(lv_obj_get_parent(*scroller));
        int sc_parent_h = lv_obj_get_height(lv_obj_get_parent(*scroller));
        // Make the width exactly equal to the highest number of thumbnails that can fit
        int sc_w = sc_parent_w - (sc_parent_w % DASH_THUMBNAIL_WIDTH);
        int sc_h = sc_parent_h - lv_obj_get_height(label_page_title) - lv_obj_get_height(label_footer);
        lv_obj_add_style(*scroller, &titleview_style, LV_PART_MAIN);
        lv_obj_align(*scroller, LV_ALIGN_TOP_MID, 0, lv_obj_get_height(label_page_title));
        lv_obj_set_width(*scroller, sc_w);
        lv_obj_set_height(*scroller, sc_h);
        // Use flex layout, so new titles automatically get positioned nicely.
        lv_obj_set_layout(*scroller, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(*scroller, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_update_layout(*scroller);

        // Create atleast ONE item in the scroller. This is just a null item when nothing is present
        title_t *t = lv_mem_alloc(sizeof(title_t));
        t->db_id = -1;
        strcpy(t->title, "No items found");
        lv_obj_t *null_item = lv_obj_create(*scroller);
        null_item->user_data = t;
        lv_obj_add_flag(null_item, LV_OBJ_FLAG_HIDDEN);
        lv_group_add_obj(lv_group_get_default(), null_item);
        lv_obj_add_event_cb(null_item, item_selection_callback, LV_EVENT_KEY, NULL);
        lv_obj_add_event_cb(null_item, item_selection_callback, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(null_item, item_selection_callback, LV_EVENT_DEFOCUSED, NULL);

        // Start a thread that starts reading the database for items on this page.
        // Thread needs to have a mutex on the database and lvgl
        parser->db_scan_thread = SDL_CreateThread(db_scan_thread_f, "game_parser_thread", parser);
    }
}

const char *dash_scroller_get_title(int index)
{
    if (index >= DASH_MAX_PAGES)
    {
        return NULL;
    }
    if (parsers[index] == NULL)
    {
        return NULL;
    }
    return parsers[index]->page_title;
}

int dash_scroller_get_page_count()
{
    return lv_obj_get_child_cnt(page_tiles);
}

bool dash_scroller_get_sort_value(const char *page_title, int *sort_value)
{
    if (page_title == NULL || sort_value == NULL)
    {
        return false;
    }

    const char* valueStart = strstr(settings_page_sorts_str, page_title);
    if (valueStart == NULL)
    {
        return false;
    }

    // Move past the parameter name and '=' sign
    valueStart += strlen(page_title) + 1;

    int value;
    if (sscanf(valueStart, "%d", &value) != 1)
    {
        return false;
    }

    value = LV_CLAMP(0, value, DASH_SORT_MAX - 1);  
    *sort_value = value;
    return true;
}

struct resort_param
{
    int sort_index;
    lv_obj_t **sorted_objs;
};

static int resort_page_callback(void *param, int argc, char **argv, char **azColName)
{
    (void) param;
    (void) azColName;
    (void) argc;

    assert(argc == 1);

    struct resort_param *p = param;
    lv_obj_t *scroller = p->sorted_objs[0];
    int db_id = atoi(argv[0]);
    lv_task_handler();
    for (unsigned int i = 1; i < lv_obj_get_child_cnt(scroller); i++)
    {
        lv_obj_t *item_container = lv_obj_get_child(scroller, i);
        //assert(lv_obj_is_valid(item_container));
        title_t *t = item_container->user_data;
        if (t->db_id == db_id)
        {
            p->sorted_objs[p->sort_index] = item_container;
            p->sort_index++;
            return 0;
        }
    }
    assert(0);
    return 0;
}

void dash_scroller_resort_page(const char *page_title)
{
    char cmd[SQL_MAX_COMMAND_LEN];
    int sort_index;
    if (dash_scroller_get_sort_value(page_title, &sort_index) == false)
    {
        return;
    }
    
    lv_obj_t *scroller = NULL;
    for (int i = 0; i < DASH_MAX_PAGES; i++)
    {
        if (parsers[i] == NULL)
        {
            continue;
        }
        if (strcmp(page_title, parsers[i]->page_title) == 0)
        {
            scroller = parsers[i]->scroller;
            break;
        }
    }
    assert(scroller);

    // If the scroller has no items leave. 1st item is a null item
    if (lv_obj_get_child_cnt(scroller) <= 1)
    {
        return;
    }

    const char *sort_by;
    const char *order_by;
    dash_scroller_get_sort_strings(sort_index, &sort_by, &order_by);

    lv_snprintf(cmd, sizeof(cmd), SQL_TITLE_GET_SORTED_LIST, SQL_TITLE_DB_ID, page_title, sort_by, order_by);

    int child_cnt = lv_obj_get_child_cnt(scroller);

    struct resort_param *p = lv_mem_alloc(sizeof(struct resort_param));
    p->sort_index = 1;
    p->sorted_objs = lv_mem_alloc(sizeof(lv_obj_t *) * child_cnt);
    
    lv_memset(p->sorted_objs, 0, sizeof(lv_obj_t *) * child_cnt);
    p->sorted_objs[0] = scroller;

    db_command_with_callback(cmd, resort_page_callback, p);
    for (int i = 1; i < child_cnt; i++)
    {
        scroller->spec_attr->children[i] = p->sorted_objs[i];
    }
    lv_mem_free(p->sorted_objs);
    lv_mem_free(p);
    lv_obj_mark_layout_as_dirty(scroller);
}