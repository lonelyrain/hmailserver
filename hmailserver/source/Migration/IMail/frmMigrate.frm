VERSION 5.00
Begin VB.Form frmMigrate 
   BorderStyle     =   1  'Fixed Single
   Caption         =   "IMail to hMailServer"
   ClientHeight    =   5130
   ClientLeft      =   45
   ClientTop       =   360
   ClientWidth     =   7920
   Icon            =   "frmMigrate.frx":0000
   LinkTopic       =   "Form1"
   MaxButton       =   0   'False
   ScaleHeight     =   5130
   ScaleWidth      =   7920
   StartUpPosition =   3  'Windows Default
   Begin IMailMigrate.Wizard Wizard1 
      Height          =   5055
      Left            =   0
      TabIndex        =   0
      Top             =   0
      Width           =   8055
      _ExtentX        =   14208
      _ExtentY        =   8916
   End
End
Attribute VB_Name = "frmMigrate"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit


Private Sub Form_Load()
   
   LoadLanguage
   
   Set m_obApp = CreateObject("hMailServer.Application")
   If AuthenticateUser(m_obApp) = False Then
      End
   End If

   Wizard1.AddPage "IMailMigrate.PageSelectFile"
   Wizard1.AddPage "IMailMigrate.PagePreview"
   Wizard1.AddPage "IMailMigrate.PageProgress"
   Wizard1.Start

   
End Sub

Private Sub Form_Resize()
   Wizard1.Width = Me.Width
   Wizard1.Height = Me.Height
End Sub


Private Sub Wizard1_WizardComplete()
      
   
   
   End
End Sub
