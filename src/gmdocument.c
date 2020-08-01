#include "gmdocument.h"
#include "gmutil.h"
#include "ui/color.h"
#include "ui/text.h"
#include "ui/metrics.h"
#include "ui/window.h"
#include "history.h"
#include "app.h"

#include <the_Foundation/ptrarray.h>
#include <the_Foundation/regexp.h>

#include <SDL_hints.h>
#include <SDL_render.h>
#include <stb_image.h>

iDeclareType(GmLink)

struct Impl_GmLink {
    iString url;
    iTime when;
    int flags;
};

void init_GmLink(iGmLink *d) {
    init_String(&d->url);
    iZap(d->when);
    d->flags = 0;
}

void deinit_GmLink(iGmLink *d) {
    deinit_String(&d->url);
}

iDefineTypeConstruction(GmLink)

iDeclareType(GmImage)

struct Impl_GmImage {
    iInt2        size;
    size_t       numBytes;
    iString      mime;
    iGmLinkId    linkId;
    SDL_Texture *texture;
};

void init_GmImage(iGmImage *d, const iBlock *data) {
    init_String(&d->mime);
    d->numBytes = size_Block(data);
    uint8_t *imgData = stbi_load_from_memory(
        constData_Block(data), size_Block(data), &d->size.x, &d->size.y, NULL, 4);
    if (!imgData) {
        d->size = zero_I2();
        d->texture = NULL;
    }
    else {
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
            imgData, d->size.x, d->size.y, 32, d->size.x * 4, SDL_PIXELFORMAT_ABGR8888);
        /* TODO: In multiwindow case, all windows must have the same shared renderer?
           Or at least a shared context. */
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); /* linear scaling */
        d->texture = SDL_CreateTextureFromSurface(renderer_Window(get_Window()), surface);
        SDL_FreeSurface(surface);
        stbi_image_free(imgData);
    }
    d->linkId = 0;
}

void deinit_GmImage(iGmImage *d) {
    SDL_DestroyTexture(d->texture);
    deinit_String(&d->mime);
}

iDefineTypeConstructionArgs(GmImage, (const iBlock *data), data)

/*----------------------------------------------------------------------------------------------*/

struct Impl_GmDocument {
    iObject object;
    enum iGmDocumentFormat format;
    iString source;
    iString url; /* for resolving relative links */
    iString localHost;
    iInt2 size;
    iArray layout; /* contents of source, laid out in document space */
    iPtrArray links;
    iString title; /* the first top-level title */
    iPtrArray images; /* persistent across layouts, references links by ID */
};

iDefineObjectConstruction(GmDocument)

enum iGmLineType {
    text_GmLineType,
    bullet_GmLineType,
    preformatted_GmLineType,
    quote_GmLineType,
    header1_GmLineType,
    header2_GmLineType,
    header3_GmLineType,
    link_GmLineType,
    max_GmLineType,
};

static enum iGmLineType lineType_GmDocument_(const iGmDocument *d, const iRangecc *line) {
    if (d->format == plainText_GmDocumentFormat) {
        return text_GmLineType;
    }
    if (isEmpty_Range(line)) {
        return text_GmLineType;
    }
    if (startsWith_Rangecc(line, "=>")) {
        return link_GmLineType;
    }
    if (startsWith_Rangecc(line, "###")) {
        return header3_GmLineType;
    }
    if (startsWith_Rangecc(line, "##")) {
        return header2_GmLineType;
    }
    if (startsWith_Rangecc(line, "#")) {
        return header1_GmLineType;
    }
    if (startsWith_Rangecc(line, "```")) {
        return preformatted_GmLineType;
    }
    if (*line->start == '>') {
        return quote_GmLineType;
    }
    if (size_Range(line) >= 2 && line->start[0] == '*' && isspace(line->start[1])) {
        return bullet_GmLineType;
    }
    return text_GmLineType;
}

static void trimLine_Rangecc_(iRangecc *line, enum iGmLineType type) {
    static const unsigned int skip[max_GmLineType] = { 0, 2, 3, 1, 1, 2, 3, 0 };
    line->start += skip[type];
    trim_Rangecc(line);
}

static int lastVisibleRunBottom_GmDocument_(const iGmDocument *d) {
    iReverseConstForEach(Array, i, &d->layout) {
        const iGmRun *run = i.value;
        if (isEmpty_Range(&run->text)) {
            continue;
        }
        return bottom_Rect(run->bounds);
    }
    return 0;
}

iInt2 measurePreformattedBlock_GmDocument_(const iGmDocument *d, const char *start, int font) {
    const iRangecc content = { start, constEnd_String(&d->source) };
    iRangecc line = iNullRange;
    nextSplit_Rangecc(&content, "\n", &line);
    iAssert(startsWith_Rangecc(&line, "```"));
    iRangecc preBlock = { line.end + 1, line.end + 1 };
    while (nextSplit_Rangecc(&content, "\n", &line)) {
        if (startsWith_Rangecc(&line, "```")) {
            break;
        }
        preBlock.end = line.end;
    }
    return measureRange_Text(font, preBlock);
}

static iRangecc addLink_GmDocument_(iGmDocument *d, iRangecc line, iGmLinkId *linkId) {
    iRegExp *pattern = new_RegExp("=>\\s*([^\\s]+)(\\s.*)?", caseInsensitive_RegExpOption);
    iRegExpMatch m;
    if (matchRange_RegExp(pattern, line, &m)) {
        iGmLink *link = new_GmLink();
        setRange_String(&link->url, capturedRange_RegExpMatch(&m, 1));
        set_String(&link->url, absoluteUrl_String(&d->url, &link->url));
        /* Check the URL. */ {
            iUrl parts;
            init_Url(&parts, &link->url);
            if (!equalCase_Rangecc(&parts.host, cstr_String(&d->localHost))) {
                link->flags |= remote_GmLinkFlag;
            }
            if (startsWithCase_Rangecc(&parts.protocol, "gemini")) {
                link->flags |= gemini_GmLinkFlag;
            }
            else if (startsWithCase_Rangecc(&parts.protocol, "http")) {
                link->flags |= http_GmLinkFlag;
            }
            else if (equalCase_Rangecc(&parts.protocol, "gopher")) {
                link->flags |= gopher_GmLinkFlag;
            }
            else if (equalCase_Rangecc(&parts.protocol, "file")) {
                link->flags |= file_GmLinkFlag;
            }
            else if (equalCase_Rangecc(&parts.protocol, "data")) {
                link->flags |= data_GmLinkFlag;
            }
            /* Check the file name extension, if present. */
            if (!isEmpty_Range(&parts.path)) {
                iString *path = newRange_String(parts.path);
                if (endsWithCase_String(path, ".gif")  || endsWithCase_String(path, ".jpg") ||
                    endsWithCase_String(path, ".jpeg") || endsWithCase_String(path, ".png") ||
                    endsWithCase_String(path, ".tga")  || endsWithCase_String(path, ".psd") ||
                    endsWithCase_String(path, ".hdr")  || endsWithCase_String(path, ".pic")) {
                    link->flags |= imageFileExtension_GmLinkFlag;
                }
                else if (endsWithCase_String(path, ".mp3") || endsWithCase_String(path, ".wav") ||
                         endsWithCase_String(path, ".mid")) {
                    link->flags |= audioFileExtension_GmLinkFlag;
                }
                delete_String(path);
            }
            /* Check if visited. */
            if (cmpString_String(&link->url, &d->url)) {
                link->when = urlVisitTime_History(history_App(), &link->url);
                if (isValid_Time(&link->when)) {
                    link->flags |= visited_GmLinkFlag;
                }
            }
        }
        pushBack_PtrArray(&d->links, link);
        *linkId = size_PtrArray(&d->links); /* index + 1 */
        iRangecc desc = capturedRange_RegExpMatch(&m, 2);
        trim_Rangecc(&desc);
        if (!isEmpty_Range(&desc)) {
            line = desc; /* Just show the description. */
            link->flags |= userFriendly_GmLinkFlag;
        }
        else {
            line = capturedRange_RegExpMatch(&m, 1); /* Show the URL. */
        }
    }
    iRelease(pattern);
    return line;
}

static void clearLinks_GmDocument_(iGmDocument *d) {
    iForEach(PtrArray, i, &d->links) {
        delete_GmLink(i.ptr);
    }
    clear_PtrArray(&d->links);
}

static size_t findLinkImage_GmDocument_(const iGmDocument *d, iGmLinkId linkId) {
    /* TODO: use a hash */
    iConstForEach(PtrArray, i, &d->images) {
        const iGmImage *img = i.ptr;
        if (img->linkId == linkId) {
            return index_PtrArrayConstIterator(&i);
        }
    }
    return iInvalidPos;
}

static void doLayout_GmDocument_(iGmDocument *d) {
    /* TODO: Collect these parameters into a GmTheme. */
    static const int fonts[max_GmLineType] = {
        paragraph_FontId,
        paragraph_FontId, /* bullet */
        preformatted_FontId,
        quote_FontId,
        header1_FontId,
        header2_FontId,
        header3_FontId,
        regular_FontId,
    };
    static const int colors[max_GmLineType] = {
        gray75_ColorId,
        gray75_ColorId,
        cyan_ColorId,
        gray75_ColorId,
        white_ColorId,
        white_ColorId,
        white_ColorId,
        white_ColorId,
    };
    static const int indents[max_GmLineType] = {
        5, 10, 5, 10, 0, 0, 0, 5
    };
    static const float topMargin[max_GmLineType] = {
        0.0f, 0.5f, 1.0f, 0.5f, 2.0f, 2.0f, 1.5f, 1.0f
    };
    static const float bottomMargin[max_GmLineType] = {
        0.0f, 0.5f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f
    };
    static const char *arrow  = "\u2192";
    static const char *bullet = "\u2022";
    static const char *folder = "\U0001f4c1";
    static const char *globe  = "\U0001f310";
    const float midRunSkip = 0.1f; /* extra space between wrapped text/quote lines */
    clear_Array(&d->layout);
    clearLinks_GmDocument_(d);
    clear_String(&d->title);
    if (d->size.x <= 0 || isEmpty_String(&d->source)) {
        return;
    }
    const iRangecc   content     = range_String(&d->source);
    iInt2            pos         = zero_I2();
    iRangecc         line        = iNullRange;
    iRangecc         preAltText  = iNullRange;
    enum iGmLineType prevType    = text_GmLineType;
    iBool            isPreformat = iFalse;
    iBool            isFirstText = iTrue;
    int              preFont     = preformatted_FontId;
    if (d->format == plainText_GmDocumentFormat) {
        isPreformat = iTrue;
        isFirstText = iFalse;
    }
    while (nextSplit_Rangecc(&content, "\n", &line)) {
        iGmRun run;
        run.flags = 0;
        run.color = white_ColorId;
        run.linkId = 0;
        run.imageId = 0;
        enum iGmLineType type;
        int indent = 0;
        if (!isPreformat) {
            type = lineType_GmDocument_(d, &line);
            indent = indents[type];
            if (type == preformatted_GmLineType) {
                isPreformat = iTrue;
                preFont = preformatted_FontId;
                /* Use a smaller font if the block contents are wide. */
                if (measurePreformattedBlock_GmDocument_(d, line.start, preFont).x >
                    d->size.x - indents[preformatted_GmLineType]) {
                    preFont = preformattedSmall_FontId;
                }
                trimLine_Rangecc_(&line, type);
                preAltText = line;
                /* TODO: store and link the alt text to this run */
                continue;
            }
            else if (type == link_GmLineType) {
                line = addLink_GmDocument_(d, line, &run.linkId);
                if (!run.linkId) {
                    /* Invalid formatting. */
                    type = text_GmLineType;
                }
            }
            trimLine_Rangecc_(&line, type);
            run.font = fonts[type];
        }
        else {
            /* Preformatted line. */
            type = preformatted_GmLineType;
            if (d->format == gemini_GmDocumentFormat &&
                startsWithSc_Rangecc(&line, "```", &iCaseSensitive)) {
                isPreformat = iFalse;
                preAltText = iNullRange;
                continue;
            }
            run.font = preFont;
            indent = indents[type];
        }
        /* Empty lines don't produce text runs. */
        if (isEmpty_Range(&line)) {
            pos.y += lineHeight_Text(run.font);
            prevType = text_GmLineType;
            /* TODO: Extra skip needed here? */
            continue;
        }
        /* Check the margin vs. previous run. */
        if (!isPreformat || (prevType != preformatted_GmLineType)) {
            int required =
                iMax(topMargin[type], bottomMargin[prevType]) * lineHeight_Text(paragraph_FontId);
            if ((type == link_GmLineType && prevType == link_GmLineType) ||
                (type == quote_GmLineType && prevType == quote_GmLineType)) {
                /* No margin between consecutive links/quote lines. */
                required =
                    (type == link_GmLineType ? midRunSkip * lineHeight_Text(paragraph_FontId) : 0);
            }
            if (isEmpty_Array(&d->layout)) {
                required = 0; /* top of document */
            }
            int delta = pos.y - lastVisibleRunBottom_GmDocument_(d);
            if (delta < required) {
                pos.y += required - delta;
            }
        }
        /* Save the document title. */
        if (type == header1_GmLineType && isEmpty_String(&d->title)) {
            setRange_String(&d->title, line);
        }
        /* List bullet. */
        run.color = colors[type];
        if (type == bullet_GmLineType) {
            run.visBounds.pos  = addX_I2(pos, indent * gap_UI);
            run.visBounds.size = advance_Text(run.font, bullet);
            run.visBounds.pos.x -= 4 * gap_UI - width_Rect(run.visBounds) / 2;
            run.bounds = zero_Rect(); /* just visual */
            run.text = range_CStr(bullet);
            pushBack_Array(&d->layout, &run);
        }
        /* Link icon. */
        if (type == link_GmLineType) {
            run.visBounds.pos  = pos;
            run.visBounds.size = init_I2(indent * gap_UI, lineHeight_Text(run.font));
            run.bounds         = zero_Rect(); /* just visual */
            const iGmLink *link = constAt_PtrArray(&d->links, run.linkId - 1);
            run.text            = range_CStr(link->flags & file_GmLinkFlag
                                      ? folder
                                      : link->flags & remote_GmLinkFlag ? globe : arrow);
            if (link->flags & remote_GmLinkFlag) {
                run.visBounds.pos.x -= gap_UI / 2;
            }
            run.color = linkColor_GmDocument(d, run.linkId);
            if (link->flags & visited_GmLinkFlag) {
                run.color--; /* darker */
            }
            pushBack_Array(&d->layout, &run);
        }
        run.color = colors[type];
        if (d->format == plainText_GmDocumentFormat) {
            run.color = colors[text_GmLineType];
        }
        /* Special formatting for the first paragraph (e.g., subtitle, introduction, or lede). */
        if (type == text_GmLineType && isFirstText) {
            run.font = firstParagraph_FontId;
            run.color = gray88_ColorId;
            isFirstText = iFalse;
        }
        else if (type != header1_GmLineType) {
            isFirstText = iFalse;
        }
        iRangecc runLine = line;
        /* Create one or more text runs for this line. */
        run.flags |= startOfLine_GmRunFlag;
        iAssert(!isEmpty_Range(&runLine)); /* must have something at this point */
        while (!isEmpty_Range(&runLine)) {
            /* Little bit of breathing space between wrapped lines. */
            if ((type == text_GmLineType || type == quote_GmLineType ||
                 type == bullet_GmLineType) &&
                runLine.start != line.start) {
                pos.y += midRunSkip * lineHeight_Text(run.font);
            }
            run.bounds.pos = addX_I2(pos, indent * gap_UI);
            const char *contPos;
            const int avail = d->size.x - run.bounds.pos.x;
            const iInt2 dims =
                tryAdvance_Text(run.font, runLine, isPreformat ? 0 : avail, &contPos);
            run.bounds.size.x = iMax(avail, dims.x); /* Extends to the right edge for selection. */
            run.bounds.size.y = dims.y;
            run.visBounds = run.bounds;
            run.visBounds.size.x = dims.x;
            if (contPos > runLine.start) {
                run.text = (iRangecc){ runLine.start, contPos };
            }
            else {
                run.text = runLine;
                contPos = runLine.end;
            }
            pushBack_Array(&d->layout, &run);
            run.flags &= ~startOfLine_GmRunFlag;
            runLine.start = contPos;
            trimStart_Rangecc(&runLine);
            pos.y += lineHeight_Text(run.font);
        }
        /* Flag the end of line, too. */
        ((iGmRun *) back_Array(&d->layout))->flags |= endOfLine_GmRunFlag;
        /* Image content. */
        if (type == link_GmLineType) {
            const size_t imgIndex = findLinkImage_GmDocument_(d, run.linkId);
            if (imgIndex != iInvalidPos) {
                ((iGmLink *) at_PtrArray(&d->links, run.linkId - 1))->flags |= content_GmLinkFlag;
                const iGmImage *img = constAt_PtrArray(&d->images, imgIndex);
                const int margin = 0.5f * lineHeight_Text(paragraph_FontId);
                pos.y += margin;
                run.bounds.pos = pos;
                run.bounds.size.x = d->size.x;
                const float aspect = (float) img->size.y / (float) img->size.x;
                run.bounds.size.y = d->size.x * aspect;
                run.visBounds = run.bounds;
                const iInt2 maxSize = mulf_I2(img->size, get_Window()->pixelRatio);
                if (width_Rect(run.visBounds) > maxSize.x) {
                    /* Don't scale the image up. */
                    run.visBounds.size.y = run.visBounds.size.y * maxSize.x / width_Rect(run.visBounds);
                    run.visBounds.size.x = maxSize.x;
                    run.visBounds.pos.x = run.bounds.size.x / 2 - width_Rect(run.visBounds) / 2;
                    run.bounds.size.y = run.visBounds.size.y;
                }
                run.text = iNullRange;
                run.font = 0;
                run.color = 0;
                run.imageId = imgIndex + 1;
                pushBack_Array(&d->layout, &run);
                pos.y += run.bounds.size.y + margin;
            }
        }
        prevType = type;
    }
    d->size.y = pos.y;
}

void init_GmDocument(iGmDocument *d) {
    d->format = gemini_GmDocumentFormat;
    init_String(&d->source);
    init_String(&d->url);
    init_String(&d->localHost);
    d->size = zero_I2();
    init_Array(&d->layout, sizeof(iGmRun));
    init_PtrArray(&d->links);
    init_String(&d->title);
    init_PtrArray(&d->images);
}

void deinit_GmDocument(iGmDocument *d) {
    deinit_String(&d->title);
    clearLinks_GmDocument_(d);
    deinit_PtrArray(&d->links);
    deinit_Array(&d->layout);
    deinit_String(&d->localHost);
    deinit_String(&d->url);
    deinit_String(&d->source);
}

void reset_GmDocument(iGmDocument *d) {
    /* Free the loaded images. */
    iForEach(PtrArray, i, &d->images) {
        deinit_GmImage(i.ptr);
    }
    clear_PtrArray(&d->images);
    clearLinks_GmDocument_(d);
    clear_Array(&d->layout);
    clear_String(&d->url);
    clear_String(&d->localHost);
}

void setFormat_GmDocument(iGmDocument *d, enum iGmDocumentFormat format) {
    d->format = format;
}

void setWidth_GmDocument(iGmDocument *d, int width) {
    d->size.x = width;
    doLayout_GmDocument_(d); /* TODO: just flag need-layout and do it later */
}

iLocalDef iBool isNormalizableSpace_(char ch) {
    return ch == ' ' || ch == '\t';
}

static void normalize_GmDocument(iGmDocument *d) {
    iString *normalized = new_String();
    iRangecc src = range_String(&d->source);
    iRangecc line = iNullRange;
    iBool isPreformat = iFalse;
    if (d->format == plainText_GmDocumentFormat) {
        isPreformat = iTrue; /* Cannot be turned off. */
    }
    const int preTabWidth = 8; /* TODO: user-configurable parameter */
    while (nextSplit_Rangecc(&src, "\n", &line)) {
        if (isPreformat) {
            /* Replace any tab characters with spaces for visualization. */
            for (const char *ch = line.start; ch != line.end; ch++) {
                if (*ch == '\t') {
                    int column = ch - line.start;
                    int numSpaces = (column / preTabWidth + 1) * preTabWidth - column;
                    while (numSpaces-- > 0) {
                        appendCStrN_String(normalized, " ", 1);
                    }
                }
                else if (*ch != '\r') {
                    appendCStrN_String(normalized, ch, 1);
                }
            }
            appendCStr_String(normalized, "\n");
            if (lineType_GmDocument_(d, &line) == preformatted_GmLineType) {
                isPreformat = iFalse;
            }
            continue;
        }
        if (lineType_GmDocument_(d, &line) == preformatted_GmLineType) {
            isPreformat = iTrue;
            appendRange_String(normalized, line);
            appendCStr_String(normalized, "\n");
            continue;
        }
        iBool isPrevSpace = iFalse;
        for (const char *ch = line.start; ch != line.end; ch++) {
            char c = *ch;
            if (c == '\r') continue;
            if (isNormalizableSpace_(c)) {
                if (isPrevSpace) {
                    continue; /* skip repeated spaces */
                }
                c = ' ';
                isPrevSpace = iTrue;
            }
            else {
                isPrevSpace = iFalse;
            }
            appendCStrN_String(normalized, &c, 1);
        }
        appendCStr_String(normalized, "\n");
    }
    set_String(&d->source, collect_String(normalized));
}

void setUrl_GmDocument(iGmDocument *d, const iString *url) {
    set_String(&d->url, url);
    iUrl parts;
    init_Url(&parts, url);
    setRange_String(&d->localHost, parts.host);
}

void setSource_GmDocument(iGmDocument *d, const iString *source, int width) {
    set_String(&d->source, source);
    normalize_GmDocument(d);
    setWidth_GmDocument(d, width);
    /* TODO: just flag need-layout and do it later */
}

void setImage_GmDocument(iGmDocument *d, iGmLinkId linkId, const iString *mime, const iBlock *data) {
    if (!mime || !data) {
        iGmImage *img;
        if (take_PtrArray(&d->images, findLinkImage_GmDocument_(d, linkId), (void **) &img)) {
            delete_GmImage(img);
        }
    }
    else {
        /* TODO: check if we know this MIME type */
        /* Load the image. */ {
            iGmImage *img = new_GmImage(data);
            img->linkId = linkId; /* TODO: use a hash? */
            set_String(&img->mime, mime);
            if (img->texture) {
                pushBack_PtrArray(&d->images, img);
            }
            else {
                delete_GmImage(img);
            }
        }
    }
    doLayout_GmDocument_(d);
}

void render_GmDocument(const iGmDocument *d, iRangei visRangeY, iGmDocumentRenderFunc render,
                       void *context) {
    iBool isInside = iFalse;
    /* TODO: Check lookup table for quick starting position. */
    iConstForEach(Array, i, &d->layout) {
        const iGmRun *run = i.value;
        if (isInside) {
            if (top_Rect(run->visBounds) > visRangeY.end) {
                break;
            }
            render(context, run);
        }
        else if (bottom_Rect(run->visBounds) >= visRangeY.start) {
            isInside = iTrue;
            render(context, run);
        }
    }
}

iInt2 size_GmDocument(const iGmDocument *d) {
    return d->size;
}

iRangecc findText_GmDocument(const iGmDocument *d, const iString *text, const char *start) {
    const char * src      = constBegin_String(&d->source);
    const size_t startPos = (start ? start - src : 0);
    const size_t pos =
        indexOfCStrFromSc_String(&d->source, cstr_String(text), startPos, &iCaseInsensitive);
    if (pos == iInvalidPos) {
        return iNullRange;
    }
    return (iRangecc){ src + pos, src + pos + size_String(text) };
}

iRangecc findTextBefore_GmDocument(const iGmDocument *d, const iString *text, const char *before) {
    iRangecc found = iNullRange;
    const char *start = constBegin_String(&d->source);
    if (!before) before = constEnd_String(&d->source);
    while (start < before) {
        iRangecc range = findText_GmDocument(d, text, start);
        if (range.start == NULL || range.start >= before) break;
        found = range;
        start = range.end;
    }
    return found;
}

const iGmRun *findRun_GmDocument(const iGmDocument *d, iInt2 pos) {
    /* TODO: Perf optimization likely needed; use a block map? */
    iConstForEach(Array, i, &d->layout) {
        const iGmRun *run = i.value;
        if (contains_Rect(run->bounds, pos)) {
            return run;
        }
    }
    return NULL;
}

const char *findLoc_GmDocument(const iGmDocument *d, iInt2 pos) {
    const iGmRun *run = findRun_GmDocument(d, pos);
    if (run) {
        return findLoc_GmRun(run, pos);
    }
    return NULL;
}

const iGmRun *findRunAtLoc_GmDocument(const iGmDocument *d, const char *textCStr) {
    iConstForEach(Array, i, &d->layout) {
        const iGmRun *run = i.value;
        if (contains_Range(&run->text, textCStr) || run->text.start > textCStr /* went past */) {
            return run;
        }
    }
    return NULL;
}

static const iGmLink *link_GmDocument_(const iGmDocument *d, iGmLinkId id) {
    if (id > 0 && id <= size_PtrArray(&d->links)) {
        return constAt_PtrArray(&d->links, id - 1);
    }
    return NULL;
}

const iString *linkUrl_GmDocument(const iGmDocument *d, iGmLinkId linkId) {
    const iGmLink *link = link_GmDocument_(d, linkId);
    return link ? &link->url : NULL;
}

int linkFlags_GmDocument(const iGmDocument *d, iGmLinkId linkId) {
    const iGmLink *link = link_GmDocument_(d, linkId);
    return link ? link->flags : 0;
}

const iTime *linkTime_GmDocument(const iGmDocument *d, iGmLinkId linkId) {
    const iGmLink *link = link_GmDocument_(d, linkId);
    return link ? &link->when : NULL;
}


uint16_t linkImage_GmDocument(const iGmDocument *d, iGmLinkId linkId) {
    size_t index = findLinkImage_GmDocument_(d, linkId);
    if (index != iInvalidPos) {
        return index + 1;
    }
    return 0;
}

enum iColorId linkColor_GmDocument(const iGmDocument *d, iGmLinkId linkId) {
    const iGmLink *link = link_GmDocument_(d, linkId);
    if (link) {
        if ((link->flags & supportedProtocol_GmLinkFlag) == 0) {
            return red_ColorId;
        }
        return link->flags & http_GmLinkFlag
                   ? orange_ColorId
                   : link->flags & gopher_GmLinkFlag ? blue_ColorId : cyan_ColorId;
    }
    return white_ColorId;
}

iBool isMediaLink_GmDocument(const iGmDocument *d, iGmLinkId linkId) {
    return (linkFlags_GmDocument(d, linkId) &
            (imageFileExtension_GmLinkFlag | audioFileExtension_GmLinkFlag)) != 0;
}

SDL_Texture *imageTexture_GmDocument(const iGmDocument *d, uint16_t imageId) {
    if (imageId > 0 && imageId <= size_PtrArray(&d->images)) {
        const iGmImage *img = constAt_PtrArray(&d->images, imageId - 1);
        return img->texture;
    }
    return NULL;
}

void imageInfo_GmDocument(const iGmDocument *d, uint16_t imageId, iGmImageInfo *info_out) {
    if (imageId > 0 && imageId <= size_PtrArray(&d->images)) {
        const iGmImage *img = constAt_PtrArray(&d->images, imageId - 1);
        info_out->size = img->size;
        info_out->numBytes = img->numBytes;
        info_out->mime = cstr_String(&img->mime);
    }
    else {
        iZap(*info_out);
    }
}

const iString *title_GmDocument(const iGmDocument *d) {
    return &d->title;
}

const char *findLoc_GmRun(const iGmRun *d, iInt2 pos) {
    const int x = pos.x - left_Rect(d->bounds);
    const char *loc;
    tryAdvanceNoWrap_Text(d->font, d->text, x, &loc);
    return loc;
}

iDefineClass(GmDocument)
