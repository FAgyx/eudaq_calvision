#include "RunControlModel.hh"

#include <vector>
#include <string>
#include "qmetatype.h"

std::vector<QString> RunControlModel::m_str_header={"type", "name", "state", "connection", "message", "information"};

namespace {
bool hideFromConnectionInformation(const std::string &connection_type,
                                   const std::string &tag) {
  const bool data_collector_tag =
      connection_type == "DataCollector" &&
      (tag == "MonitorEventN" || tag == "SyncDropN" || tag == "_SERVER");
  return data_collector_tag ||
         tag == "FERS_BOARD_COUNT" ||
         tag == "FERS_INFO" ||
         tag.rfind("FERS_HV_", 0) == 0 ||
         tag.rfind("FERS_BRD", 0) == 0 ||
         tag.rfind("Fast", 0) == 0;
}
}

RunControlModel::RunControlModel(QObject *parent)
  : QAbstractListModel(parent){
}

void RunControlModel::newconnection(eudaq::ConnectionSPC id){
  m_con_status[id].reset();  
  beginInsertRows(QModelIndex(), 0, 0);
  endInsertRows();
}

void RunControlModel::disconnected(eudaq::ConnectionSPC id){
  m_con_status.erase(id);
  emit dataChanged(createIndex(0, 0), createIndex(m_con_status.size(), m_str_header.size()-1));
}

void RunControlModel::SetStatus(eudaq::ConnectionSPC id,
                                eudaq::StatusSPC status){
  m_con_status.at(id) = status;
  emit dataChanged(createIndex(0, 0), createIndex(m_con_status.size()-1, m_str_header.size()-1));
}


int RunControlModel::rowCount(const QModelIndex & /*parent*/) const {
  return m_con_status.size();
}

int RunControlModel::columnCount(const QModelIndex & /*parent*/) const {
  return m_str_header.size();
}

QVariant RunControlModel::data(const QModelIndex &index, int role) const {
  if(role != Qt::DisplayRole || !index.isValid())
    return QVariant();

  auto col_n = index.column();
  auto row_n = index.row();
  
  if(row_n<m_con_status.size() && col_n<m_str_header.size()){
    auto it = m_con_status.begin();
    for(size_t i = 0; i< row_n; i++){
      it++;
    }
    auto &con = it->first;
    auto &sta = it->second;
    switch(col_n){
    case 0:
      return QString::fromStdString(con->GetType());
    case 1:
      return QString::fromStdString(con->GetName());
    case 2:{
      if(sta)
	return QString::fromStdString(sta->GetStateString());
      else
	return QString("WAITING: NO Status");
    }
    case 3:
      return QString::fromStdString(con->GetRemote());
    case 4:
      if(sta)
	return QString::fromStdString(sta->GetMessage());
      else
	return QString("");
    case 5:{
      std::string info;
      if(sta){
	auto tags = sta->GetTags();
	for(auto &tag: tags){
          if (hideFromConnectionInformation(con->GetType(), tag.first)) {
            continue;
          }
	  info += ("<"+tag.first+"> ");
	  info += (tag.second+"  ");
	}
      }
      return QString::fromStdString(info);
    }
    default:
      return QString("");
    }
  }
  else
    return QVariant();
}

QVariant RunControlModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const{
  if (role != Qt::DisplayRole)
    return QVariant();

  if (orientation == Qt::Horizontal && section < m_str_header.size()){
    return m_str_header[section];
  }
  return QVariant();
}

eudaq::ConnectionSPC RunControlModel::getConnection(const QModelIndex &index){
  auto col_n = index.column();
  auto row_n = index.row();
  
  if(row_n<m_con_status.size() && col_n<m_str_header.size()){
    auto it = m_con_status.begin();
    for(size_t i = 0; i< row_n; i++){
      it++;
    }
    auto &con = it->first;	
    return con;
    }
  return eudaq::ConnectionSPC();
}
