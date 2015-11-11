// This file is part of RSS Guard.
//
// Copyright (C) 2011-2015 by Martin Rotter <rotter.martinos@gmail.com>
//
// RSS Guard is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// RSS Guard is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with RSS Guard. If not, see <http://www.gnu.org/licenses/>.

#include "services/standard/standardserviceroot.h"

#include "definitions/definitions.h"
#include "miscellaneous/application.h"
#include "miscellaneous/settings.h"
#include "miscellaneous/iconfactory.h"
#include "core/feedsmodel.h"
#include "gui/messagebox.h"
#include "gui/dialogs/formmain.h"
#include "exceptions/applicationexception.h"
#include "services/standard/standardserviceentrypoint.h"
#include "services/standard/standardrecyclebin.h"
#include "services/standard/standardfeed.h"
#include "services/standard/standardcategory.h"
#include "services/standard/standardfeedsimportexportmodel.h"
#include "services/standard/gui/formstandardcategorydetails.h"
#include "services/standard/gui/formstandardfeeddetails.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QStack>
#include <QCoreApplication>
#include <QAction>
#include <QPointer>


StandardServiceRoot::StandardServiceRoot(bool load_from_db, FeedsModel *feeds_model, RootItem *parent)
  : ServiceRoot(feeds_model, parent), m_recycleBin(new StandardRecycleBin(this)),
    m_addItemMenu(QList<QAction*>()), m_feedContextMenu(QList<QAction*>()), m_actionFeedFetchMetadata(NULL) {
  setTitle(qApp->system()->getUsername() + QL1S("@") + QL1S(APP_LOW_NAME));
  setIcon(StandardServiceEntryPoint().icon());
  setDescription(tr("This is obligatory service account for standard RSS/RDF/ATOM feeds."));
  setCreationDate(QDateTime::currentDateTime());

  if (load_from_db) {
    loadFromDatabase();
  }
}

StandardServiceRoot::~StandardServiceRoot() {
  qDeleteAll(m_addItemMenu);
  qDeleteAll(m_feedContextMenu);
}

void StandardServiceRoot::start() {
  if (qApp->isFirstRun()) {
    if (MessageBox::show(qApp->mainForm(), QMessageBox::Question, QObject::tr("Load initial feeds"),
                         QObject::tr("You started %1 for the first time, now you can load initial set of feeds.").arg(APP_NAME),
                         QObject::tr("Do you want to load initial set of feeds?"),
                         QString(), QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
      QString target_opml_file = APP_INITIAL_FEEDS_PATH + QDir::separator() + FEED_INITIAL_OPML_PATTERN;
      QString current_locale = qApp->localization()->loadedLanguage();
      QString file_to_load;

      if (QFile::exists(target_opml_file.arg(current_locale))) {
        file_to_load = target_opml_file.arg(current_locale);
      }
      else if (QFile::exists(target_opml_file.arg(DEFAULT_LOCALE))) {
        file_to_load = target_opml_file.arg(DEFAULT_LOCALE);
      }

      FeedsImportExportModel model;
      QString output_msg;

      try {
        model.importAsOPML20(IOFactory::readTextFile(file_to_load));
        model.checkAllItems();

        // TODO: Expand all items here?
        mergeImportExportModel(&model, output_msg);
      }
      catch (ApplicationException &ex) {
        MessageBox::show(qApp->mainForm(), QMessageBox::Critical, tr("Error when loading initial feeds"), ex.message());
      }
    }
  }
}

void StandardServiceRoot::stop() {

}

bool StandardServiceRoot::canBeEdited() {
  return false;
}

bool StandardServiceRoot::canBeDeleted() {
  return false;
}

QVariant StandardServiceRoot::data(int column, int role) const {
  switch (role) {
    case Qt::ToolTipRole:
      if (column == FDS_MODEL_TITLE_INDEX) {
        return tr("This is service account for standard RSS/RDF/ATOM feeds.");
      }
      else if (column == FDS_MODEL_COUNTS_INDEX) {
        //: Tooltip for "unread" column of feed list.
        return tr("%n unread message(s).", 0, countOfUnreadMessages());
      }
      else {
        return QVariant();
      }

    default:
      return ServiceRoot::data(column, role);
  }
}

void StandardServiceRoot::loadFromDatabase(){
  QSqlDatabase database = qApp->database()->connection("StandardServiceRoot", DatabaseFactory::FromSettings);
  CategoryAssignment categories;
  FeedAssignment feeds;

  // Obtain data for categories from the database.
  QSqlQuery query_categories(database);
  query_categories.setForwardOnly(true);

  if (!query_categories.exec(QSL("SELECT * FROM Categories;")) || query_categories.lastError().isValid()) {
    qFatal("Query for obtaining categories failed. Error message: '%s'.",
           qPrintable(query_categories.lastError().text()));
  }

  while (query_categories.next()) {
    CategoryAssignmentItem pair;
    pair.first = query_categories.value(CAT_DB_PARENT_ID_INDEX).toInt();
    pair.second = new StandardCategory(query_categories.record());

    categories << pair;
  }

  // All categories are now loaded.
  QSqlQuery query_feeds(database);
  query_feeds.setForwardOnly(true);

  if (!query_feeds.exec(QSL("SELECT * FROM Feeds;")) || query_feeds.lastError().isValid()) {
    qFatal("Query for obtaining feeds failed. Error message: '%s'.",
           qPrintable(query_feeds.lastError().text()));
  }

  while (query_feeds.next()) {
    // Process this feed.
    StandardFeed::Type type = static_cast<StandardFeed::Type>(query_feeds.value(FDS_DB_TYPE_INDEX).toInt());

    switch (type) {
      case StandardFeed::Atom10:
      case StandardFeed::Rdf:
      case StandardFeed::Rss0X:
      case StandardFeed::Rss2X: {
        FeedAssignmentItem pair;
        pair.first = query_feeds.value(FDS_DB_CATEGORY_INDEX).toInt();
        pair.second = new StandardFeed(query_feeds.record());
        pair.second->setType(type);

        feeds << pair;
        break;
      }

      default:
        break;
    }
  }

  // All data are now obtained, lets create the hierarchy.
  assembleCategories(categories);
  assembleFeeds(feeds);

  // As the last item, add recycle bin, which is needed.
  appendChild(m_recycleBin);
}

QHash<int,StandardCategory*> StandardServiceRoot::categoriesForItem(RootItem *root) {
  QHash<int,StandardCategory*> categories;
  QList<RootItem*> parents;

  parents.append(root->childItems());

  while (!parents.isEmpty()) {
    RootItem *item = parents.takeFirst();

    if (item->kind() == RootItemKind::Category) {
      // This item is category, add it to the output list and
      // scan its children.
      int category_id = item->id();
      StandardCategory *category = static_cast<StandardCategory*>(item);

      if (!categories.contains(category_id)) {
        categories.insert(category_id, category);
      }

      parents.append(category->childItems());
    }
  }

  return categories;
}

QHash<int,StandardCategory*> StandardServiceRoot::allCategories() {
  return categoriesForItem(this);
}

QList<QAction*> StandardServiceRoot::getContextMenuForFeed(StandardFeed *feed) {
  if (m_feedContextMenu.isEmpty()) {
    // Initialize.
    m_actionFeedFetchMetadata = new QAction(qApp->icons()->fromTheme(QSL("download-manager")), tr("Fetch metadata"), NULL);
    m_feedContextMenu.append(m_actionFeedFetchMetadata);
  }

  // Make connections.
  disconnect(m_actionFeedFetchMetadata, SIGNAL(triggered()), 0, 0);
  connect(m_actionFeedFetchMetadata, SIGNAL(triggered()), feed, SLOT(fetchMetadataForItself()));

  return m_feedContextMenu;
}

void StandardServiceRoot::assembleFeeds(FeedAssignment feeds) {
  QHash<int,StandardCategory*> categories = categoriesForItem(this);

  foreach (const FeedAssignmentItem &feed, feeds) {
    if (feed.first == NO_PARENT_CATEGORY) {
      // This is top-level feed, add it to the root item.
      appendChild(feed.second);
    }
    else if (categories.contains(feed.first)) {
      // This feed belongs to this category.
      categories.value(feed.first)->appendChild(feed.second);
    }
    else {
      qWarning("Feed '%s' is loose, skipping it.", qPrintable(feed.second->title()));
    }
  }
}

StandardRecycleBin *StandardServiceRoot::recycleBin() const {
  return m_recycleBin;
}

bool StandardServiceRoot::mergeImportExportModel(FeedsImportExportModel *model, QString &output_message) {
  QStack<RootItem*> original_parents; original_parents.push(this);
  QStack<RootItem*> new_parents; new_parents.push(model->rootItem());
  bool some_feed_category_error = false;

  // Iterate all new items we would like to merge into current model.
  while (!new_parents.isEmpty()) {
    RootItem *target_parent = original_parents.pop();
    RootItem *source_parent = new_parents.pop();

    foreach (RootItem *source_item, source_parent->childItems()) {
      if (!model->isItemChecked(source_item)) {
        // We can skip this item, because it is not checked and should not be imported.
        // NOTE: All descendants are thus skipped too.
        continue;
      }

      if (source_item->kind() == RootItemKind::Category) {
        StandardCategory *source_category = static_cast<StandardCategory*>(source_item);
        StandardCategory *new_category = new StandardCategory(*source_category);
        QString new_category_title = new_category->title();

        // Add category to model.
        new_category->clearChildren();

        if (new_category->addItself(target_parent)) {
          feedsModel()->reassignNodeToNewParent(new_category, target_parent);

          // Process all children of this category.
          original_parents.push(new_category);
          new_parents.push(source_category);
        }
        else {
          delete new_category;

          // Add category failed, but this can mean that the same category (with same title)
          // already exists. If such a category exists in current parent, then find it and
          // add descendants to it.
          RootItem *existing_category = NULL;
          foreach (RootItem *child, target_parent->childItems()) {
            if (child->kind() == RootItemKind::Category && child->title() == new_category_title) {
              existing_category = child;
            }
          }

          if (existing_category != NULL) {
            original_parents.push(existing_category);
            new_parents.push(source_category);
          }
          else {
            some_feed_category_error = true;
          }
        }
      }
      else if (source_item->kind() == RootItemKind::Feed) {
        StandardFeed *source_feed = static_cast<StandardFeed*>(source_item);
        StandardFeed *new_feed = new StandardFeed(*source_feed);

        // Append this feed and end this iteration.
        if (new_feed->addItself(target_parent)) {
          feedsModel()->reassignNodeToNewParent(new_feed, target_parent);
        }
        else {
          delete new_feed;
          some_feed_category_error = true;
        }
      }
    }
  }

  if (some_feed_category_error) {
    output_message = tr("Import successfull, but some feeds/categories were not imported due to error.");
  }
  else {
    output_message = tr("Import was completely successfull.");
  }

  return !some_feed_category_error;
}

void StandardServiceRoot::addNewCategory() {
  QPointer<FormStandardCategoryDetails> form_pointer = new FormStandardCategoryDetails(this, qApp->mainForm());

  form_pointer.data()->exec(NULL, NULL);
  delete form_pointer.data();
}

void StandardServiceRoot::addNewFeed() {
  QPointer<FormStandardFeedDetails> form_pointer = new FormStandardFeedDetails(this, qApp->mainForm());

  form_pointer.data()->exec(NULL, NULL);
  delete form_pointer.data();
}

QList<QAction*> StandardServiceRoot::addItemMenu() {
  if (m_addItemMenu.isEmpty()) {
    QAction *action_new_category = new QAction(qApp->icons()->fromTheme("folder-category"), tr("Add new category"), this);
    connect(action_new_category, SIGNAL(triggered()), this, SLOT(addNewCategory()));

    QAction *action_new_feed = new QAction(qApp->icons()->fromTheme("folder-feed"), tr("Add new feed"), this);
    connect(action_new_feed, SIGNAL(triggered()), this, SLOT(addNewFeed()));

    m_addItemMenu.append(action_new_category);
    m_addItemMenu.append(action_new_feed);
  }

  return m_addItemMenu;
}

QList<QAction*> StandardServiceRoot::serviceMenu() {
  return m_addItemMenu;
}

void StandardServiceRoot::assembleCategories(CategoryAssignment categories) {
  QHash<int, RootItem*> assignments;
  assignments.insert(NO_PARENT_CATEGORY, this);

  // Add top-level categories.
  while (!categories.isEmpty()) {
    for (int i = 0; i < categories.size(); i++) {
      if (assignments.contains(categories.at(i).first)) {
        // Parent category of this category is already added.
        assignments.value(categories.at(i).first)->appendChild(categories.at(i).second);

        // Now, added category can be parent for another categories, add it.
        assignments.insert(categories.at(i).second->id(), categories.at(i).second);

        // Remove the category from the list, because it was
        // added to the final collection.
        categories.removeAt(i);
        i--;
      }
    }
  }
}